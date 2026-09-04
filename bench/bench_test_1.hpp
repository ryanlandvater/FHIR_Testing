#pragma once

#include <cstdlib>

/*
IMPORTANT BENCHMARK NOTE

This header is intentionally NOT representative of normal FastFHIR usage.

Normal FastFHIR application code should typically:
1) populate PatientData (or other resource structs) directly, then
2) call builder.append_obj(populated_struct).

In this benchmark harness, assignment is intentionally structured differently
to enforce per-line assignment parity across formats (FastFHIR vs JSON) for
fair, auditable comparisons. The shared assignment flow in this file is a
benchmark-control mechanism, not a recommended production integration pattern.
*/

#include <FF_Patient.hpp>
#include <FF_Observation.hpp>

#if defined(ARM_GOOGLE_FHIR)
#include "proto/google/fhir/proto/r4/core/codes.pb.h"
#include "proto/google/fhir/proto/r4/core/datatypes.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/observation.pb.h"
#include "proto/google/fhir/proto/r4/core/resources/patient.pb.h"

// timegm is POSIX; MSVC provides _mkgmtime instead
#ifdef _MSC_VER
// Windows lacks POSIX timegm; _mkgmtime is the equivalent.
#include <time.h>
namespace
{
  inline time_t _timegm(struct tm *tm) { return _mkgmtime(tm); }
} // namespace
#define timegm(tm) _timegm(tm)
#endif

#endif

#include <nlohmann/json.hpp>

#include <ctime>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>


// ---------------------------------------------------------------------------
// Per-arm namespace -- REQUIRED FOR CORRECTNESS, not style.
// ---------------------------------------------------------------------------
// Each arm compiles these headers with a different ARM_* macro, so the SAME
// type and function names get four DIFFERENT definitions across four
// translation units: bench::test_2::MaterializedTree holds a
// unique_ptr<FastFHIR::Parser> in one TU, a simdjson element in another, and
// two protobuf vectors in a third.
//
// That is a One Definition Rule violation. The linker keeps one definition of
// each inline function and destructor and discards the rest, so an object built
// with one layout gets destroyed with another. It manifests as heap corruption
// far from the cause -- ASan caught it as a SEGV inside
// ~vector<google::fhir::r4::core::Observation> from
// bench::test_2::MaterializedTree::~MaterializedTree, and it also moved the
// apparent crash site around between -c opt and -c dbg builds, which is the
// classic signature.
//
// An inline namespace gives each arm its own mangled symbols while leaving
// every existing call site (bench::test_2::query, bench::assign::assign_patient)
// spelled exactly as before.
#ifndef BENCH_ARM_NS
#if defined(ARM_FASTFHIR)
#define BENCH_ARM_NS arm_fastfhir
#elif defined(ARM_JSON)
#define BENCH_ARM_NS arm_json
#elif defined(ARM_HL7V2)
#define BENCH_ARM_NS arm_hl7v2
#elif defined(ARM_GOOGLE_FHIR)
#define BENCH_ARM_NS arm_google_fhir
#else
#define BENCH_ARM_NS arm_none
#endif
#endif

namespace bench::assign
{
inline namespace BENCH_ARM_NS {

#if defined(ARM_GOOGLE_FHIR)
  struct GooglePatientTarget
  {
    google::fhir::r4::core::Patient &patient;
  };

  struct GoogleObservationTarget
  {
    google::fhir::r4::core::Observation &observation;
    std::string_view fallback_patient_id;
  };
#endif

  namespace detail
  {
    // The URL table of the document being serialized (BundlePatient::url_table,
    // rebuilt from FastFHIR's trie at ingest). Installed for the duration of one
    // bundle item: the to_json_* chain is ~20 recursive converters deep, and
    // threading a resolver through every one of them to reach the single site
    // that needs it is the worse trade. RAII so a stale table can never outlive
    // the document it belongs to.
    // thread_local: arms serialize bundle items in PARALLEL (dispatch_apply),
    // and each item has its own arena, so intern index 16 names a different URL
    // in different patients. A single shared pointer here would be both a data
    // race and silently wrong.
    inline thread_local const std::vector<std::string> *g_url_table = nullptr;

    struct ScopedUrlTable {
      explicit ScopedUrlTable(const std::vector<std::string> &t) noexcept { g_url_table = &t; }
      ~ScopedUrlTable() noexcept { g_url_table = nullptr; }
      ScopedUrlTable(const ScopedUrlTable &) = delete;
      ScopedUrlTable &operator=(const ScopedUrlTable &) = delete;
    };

    inline std::string resolve_extension_url(std::uint32_t idx) {
      if (g_url_table == nullptr || idx >= g_url_table->size())
        return {};
      return (*g_url_table)[idx];
    }



    using Json = nlohmann::json;

    // ---------------------------------------------------------------------
    // Cross-arena choice[x] limitation -- READ THIS BEFORE TRUSTING TEST 1.
    // ---------------------------------------------------------------------
    // FastFHIR's generated deserializer stores a BLOCK-typed choice variant
    // (valueQuantity, valueCodeableConcept, valuePeriod, ...) as the raw child
    // OFFSET of that block, in the ChoiceEntry's uint64_t alternative:
    //
    //     generated_src/FF_Observation.cpp:319
    //         else data.value.value = child_off;   // offset into the SOURCE arena
    //
    // That offset is only meaningful inside the arena it was read from. The
    // POCO carries no base pointer, so a ChoiceEntry alone cannot be re-serialized
    // into a different arena -- and every arm here does exactly that, because the
    // fixtures are hydrated from one arena and the arms serialize into another.
    //
    // What each arm did with it before this was noticed (2026-08-25):
    //   * FastFHIR arm: wrote the foreign offset into a slot tagged as a block,
    //     producing a STRUCTURALLY CORRUPT stream. validate_FFHR_stream() reports
    //     "the offset chain is broken", and reading it back segfaults in
    //     FF_CODEABLECONCEPT::deserialize.
    //   * JSON arm: emitted {"valueQuantity": 55683} -- a raw arena offset as a
    //     JSON number. Valid JSON, meaningless FHIR.
    //   * Google/HL7v2 arms: same integer, same meaninglessness.
    //
    // So NO arm has ever serialized a real valueQuantity. Rather than have one
    // arm corrupt its stream and the others emit nonsense, every arm now SKIPS
    // block-typed choices uniformly. That keeps the arms comparable and the
    // streams valid, at the cost of Test 1 not measuring value[x] at all.
    //
    // Fixing it properly needs the source arena base plumbed into the assignment
    // sink so the block can be deep-copied (deserialize from source, append into
    // destination). That is the single biggest correctness gap in the benchmark
    // -- see notes.md, "Block-typed choice[x] cannot cross arenas".

    inline bool has_u8(uint8_t v) { return v != FF_NULL_UINT8; }
    inline bool has_u32(uint32_t v) { return v != FF_NULL_UINT32; }
    inline bool has_f64(double v) { return v != FF_NULL_F64; }

#if defined(ARM_HL7V2)
    struct HL7v2Sink
    {
      bench::hl7v2::OruR01Message &message;
      bench::hl7v2::ObxSegment current_obx{};
      bool has_open_observation = false;

      void append_custom_field(std::string_view field_name, std::string payload)
      {
        message.append_custom_field(field_name, std::move(payload));
      }

      void begin_observation()
      {
        current_obx = bench::hl7v2::ObxSegment{};
        current_obx.set_id = static_cast<int>(message.obx.size() + 1);
        has_open_observation = true;
      }

      void finish_observation()
      {
        if (!has_open_observation)
        {
          return;
        }
        if (current_obx.observation_id.empty())
        {
          current_obx.observation_id = "UNK^Observation^99LOCAL";
        }
        message.obx.push_back(std::move(current_obx));
        current_obx = bench::hl7v2::ObxSegment{};
        has_open_observation = false;
      }
    };
#endif

#if defined(ARM_GOOGLE_FHIR)
    using GoogleAdministrativeGenderValue = google::fhir::r4::core::AdministrativeGenderCode_Value;
    using GoogleDatePrecision = google::fhir::r4::core::Date_Precision;
    using GoogleObservationStatusValue = google::fhir::r4::core::ObservationStatusCode_Value;

// timegm is POSIX; MSVC provides _mkgmtime which is equivalent.
#ifdef _MSC_VER
#define timegm _mkgmtime
#endif

    inline std::optional<int64_t> google_birthdate_to_us(std::string_view birthdate)
    {
      if (birthdate.size() < 10)
      {
        return std::nullopt;
      }

      std::tm tm{};
      tm.tm_year = std::atoi(std::string(birthdate.substr(0, 4)).c_str()) - 1900;
      tm.tm_mon = std::atoi(std::string(birthdate.substr(5, 2)).c_str()) - 1;
      tm.tm_mday = std::atoi(std::string(birthdate.substr(8, 2)).c_str());
      tm.tm_hour = 0;
      tm.tm_min = 0;
      tm.tm_sec = 0;

      const std::time_t epoch_seconds = timegm(&tm);
      if (epoch_seconds < 0)
      {
        return std::nullopt;
      }
      return static_cast<int64_t>(epoch_seconds) * 1000000LL;
    }

    inline GoogleAdministrativeGenderValue google_map_gender(FF_AdministrativeGender gender)
    {
      switch (gender)
      {
      case FF_AdministrativeGender::Male:
        return google::fhir::r4::core::AdministrativeGenderCode_Value_MALE;
      case FF_AdministrativeGender::Female:
        return google::fhir::r4::core::AdministrativeGenderCode_Value_FEMALE;
      case FF_AdministrativeGender::Other:
        return google::fhir::r4::core::AdministrativeGenderCode_Value_OTHER;
      case FF_AdministrativeGender::Unknown:
      default:
        return google::fhir::r4::core::AdministrativeGenderCode_Value_UNKNOWN;
      }
    }

    // NEVER static_cast BETWEEN THESE ENUMS. FastFHIR generates its code enums
    // ALPHABETICALLY (Email, Fax, Other, Pager, Phone, Sms, Url) while FhirProto
    // numbers them in the order the FHIR ValueSet declares them (PHONE = 1,
    // FAX = 2, EMAIL = 3, PAGER = 4, ...). The two sequences agree nowhere past
    // the first entry, so a numeric cast does not fail -- it silently returns a
    // DIFFERENT, entirely valid code. `Phone` (4) arrived as `PAGER` (4). That
    // is worse than a dropped field: a missing telecom.system is visible, a
    // wrong one is a patient with a pager.
    inline ::google::fhir::r4::core::ContactPointSystemCode_Value
    google_map_contact_system(FF_ContactPointSystem v)
    {
      using G = ::google::fhir::r4::core::ContactPointSystemCode;
      switch (v)
      {
      case FF_ContactPointSystem::Phone: return G::PHONE;
      case FF_ContactPointSystem::Fax:   return G::FAX;
      case FF_ContactPointSystem::Email: return G::EMAIL;
      case FF_ContactPointSystem::Pager: return G::PAGER;
      case FF_ContactPointSystem::Url:   return G::URL;
      case FF_ContactPointSystem::Sms:   return G::SMS;
      case FF_ContactPointSystem::Other: return G::OTHER;
      default:                           return G::INVALID_UNINITIALIZED;
      }
    }

    inline ::google::fhir::r4::core::ContactPointUseCode_Value
    google_map_contact_use(FF_ContactPointUse v)
    {
      using G = ::google::fhir::r4::core::ContactPointUseCode;
      switch (v)
      {
      case FF_ContactPointUse::Home:   return G::HOME;
      case FF_ContactPointUse::Work:   return G::WORK;
      case FF_ContactPointUse::Temp:   return G::TEMP;
      case FF_ContactPointUse::Old:    return G::OLD;
      case FF_ContactPointUse::Mobile: return G::MOBILE;
      default:                         return G::INVALID_UNINITIALIZED;
      }
    }

    inline ::google::fhir::r4::core::NameUseCode_Value google_map_name_use(FF_NameUse v)
    {
      using G = ::google::fhir::r4::core::NameUseCode;
      switch (v)
      {
      case FF_NameUse::Usual:     return G::USUAL;
      case FF_NameUse::Official:  return G::OFFICIAL;
      case FF_NameUse::Temp:      return G::TEMP;
      case FF_NameUse::Nickname:  return G::NICKNAME;
      case FF_NameUse::Anonymous: return G::ANONYMOUS;
      case FF_NameUse::Old:       return G::OLD;
      case FF_NameUse::Maiden:    return G::MAIDEN;
      default:                    return G::INVALID_UNINITIALIZED;
      }
    }

    inline GoogleObservationStatusValue google_map_observation_status(FF_ObservationStatus status)
    {
      switch (status)
      {
      case FF_ObservationStatus::Registered:
        return google::fhir::r4::core::ObservationStatusCode_Value_REGISTERED;
      case FF_ObservationStatus::Preliminary:
        return google::fhir::r4::core::ObservationStatusCode_Value_PRELIMINARY;
      case FF_ObservationStatus::Final:
        return google::fhir::r4::core::ObservationStatusCode_Value_FINAL;
      case FF_ObservationStatus::Amended:
        return google::fhir::r4::core::ObservationStatusCode_Value_AMENDED;
      case FF_ObservationStatus::Corrected:
        return google::fhir::r4::core::ObservationStatusCode_Value_CORRECTED;
      case FF_ObservationStatus::Cancelled:
        return google::fhir::r4::core::ObservationStatusCode_Value_CANCELLED;
      case FF_ObservationStatus::EnteredInError:
        return google::fhir::r4::core::ObservationStatusCode_Value_ENTERED_IN_ERROR;
      case FF_ObservationStatus::Unknown:
      default:
        return google::fhir::r4::core::ObservationStatusCode_Value_UNKNOWN;
      }
    }

    inline std::string google_subject_patient_id(const ReferenceData *subject,
                                                 std::string_view fallback_patient_id)
    {
      if (subject && !subject->reference.empty())
      {
        const std::string_view ref = subject->reference;
        constexpr std::string_view kPatientPrefix = "Patient/";
        if (ref.substr(0, kPatientPrefix.size()) == kPatientPrefix && ref.size() > kPatientPrefix.size())
        {
          return std::string(ref.substr(kPatientPrefix.size()));
        }
        return std::string(ref);
      }

      if (!fallback_patient_id.empty())
      {
        return std::string(fallback_patient_id);
      }
      return {};
    }

    // Recursive helper to build generic FHIR extensions
            #endif

    inline std::string choice_suffix(RECOVERY_TAG tag)
    {
      switch (tag)
      {
      case RECOVER_FF_BOOL:
        return "Boolean";
      case RECOVER_FF_INT32:
      case RECOVER_FF_UINT32:
      case RECOVER_FF_INT64:
      case RECOVER_FF_UINT64:
        return "Integer";
      case RECOVER_FF_FLOAT64:
        return "Decimal";
      case RECOVER_FF_DATE:
        return "Date";
      case RECOVER_FF_DATETIME:
        return "DateTime";
      case RECOVER_FF_TIME:
        return "Time";
      case RECOVER_FF_INSTANT:
        return "Instant";
      case RECOVER_FF_CODE:
        return "Code";
      case RECOVER_FF_STRING:
        return "String";
      default:
        return "String";
      }
    }


    // Defined below, once the per-datatype converters it forwards to have been
    // declared. See "Rendering a DECODED block-typed choice".
    // Complete here, not forward-declared: std::optional<T> requires T complete
    // at the point the declaration below is parsed.
    struct RenderedChoice {
        std::string suffix;  // the DATATYPE name, e.g. "Quantity"
        Json        value;
    };

    inline std::optional<RenderedChoice> choice_block_json(const ChoiceBlock &blk);

#if defined(ARM_GOOGLE_FHIR)
    // ---- FhirProto encoding helpers -------------------------------------
    // Written against google/fhir's own proto definitions (vendored under
    // third_party/google_fhir) and the JSON conventions its printer implements.
    // FhirProto is not a transliteration of FHIR JSON -- primitives are wrapped
    // messages, choices are oneofs in a nested `<Field>X`, references are
    // typed, and timelike values are absolute microseconds plus a timezone and
    // a precision. Each helper below encodes one of those rules once.

    // "MedicationRequest" -> "medication_request", to find `<type>_id` on
    // Reference's oneof by name.
    inline std::string google_snake(std::string_view camel)
    {
      std::string out;
      for (std::size_t i = 0; i < camel.size(); ++i)
      {
        const char c = camel[i];
        if (std::isupper(static_cast<unsigned char>(c)))
        {
          if (i) out.push_back('_');
          out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        else out.push_back(c);
      }
      return out;
    }

    // FHIR's single `reference` string -> Reference's oneof.
    //
    // A RELATIVE reference is exactly `<ResourceType>/<id>`, and only that form
    // is typed: FhirProto stores it in the matching `<type>_id` member. Anything
    // else -- `urn:uuid:...`, an absolute URL, a `#fragment` -- is NOT a typed
    // reference and belongs in `uri` (or `fragment`), which is what google/fhir's
    // own parser does with it.
    //
    // This matters here because Synthea's bundles are urn:uuid THROUGHOUT. The
    // previous code pushed those straight into `patient_id`, so the arm claimed
    // a typed Patient reference whose id was the literal text "urn:uuid:0fb5...".
    // Printed back through FhirProto's conventions that reads
    // "Patient/urn:uuid:0fb5..." -- a reference to nothing, and 2,936 leaves
    // that disagreed with every other arm.
    inline void google_set_reference(const ReferenceData &src,
                                     google::fhir::r4::core::Reference *pb)
    {
      if (pb == nullptr) return;
      if (!src.display.empty())
        pb->mutable_display()->set_value(std::string(src.display));

      const std::string ref(src.reference);
      if (ref.empty()) return;

      if (ref[0] == '#')
      {
        pb->mutable_fragment()->set_value(ref.substr(1));
        return;
      }

      const auto slash = ref.find('/');
      const bool absolute = ref.rfind("urn:", 0) == 0 || ref.rfind("http://", 0) == 0 ||
                            ref.rfind("https://", 0) == 0;
      if (!absolute && slash != std::string::npos && slash > 0)
      {
        const std::string field = google_snake(ref.substr(0, slash)) + "_id";
        const auto *desc = pb->GetDescriptor();
        if (const auto *f = desc->FindFieldByName(field))
        {
          auto *id_msg = pb->GetReflection()->MutableMessage(pb, f);
          if (const auto *vf = id_msg->GetDescriptor()->FindFieldByName("value"))
          {
            id_msg->GetReflection()->SetString(id_msg, vf, ref.substr(slash + 1));
            return;
          }
        }
      }
      pb->mutable_uri()->set_value(ref);
    }

    // A FHIR instant/dateTime -> absolute microseconds. The old helper read only
    // the first ten characters and zeroed h/m/s, so every `issued` and every
    // `effectiveDateTime` in the corpus landed on midnight of its date -- and
    // `meta.lastUpdated` was not even that, it was the literal 0.
    inline std::optional<int64_t> google_instant_us(std::string_view text)
    {
      if (text.size() < 10) return std::nullopt;
      std::tm tm{};
      tm.tm_year = std::atoi(std::string(text.substr(0, 4)).c_str()) - 1900;
      tm.tm_mon  = std::atoi(std::string(text.substr(5, 2)).c_str()) - 1;
      tm.tm_mday = std::atoi(std::string(text.substr(8, 2)).c_str());

      int64_t frac_us = 0, offset_s = 0;
      if (text.size() >= 19 && text[10] == 'T')
      {
        tm.tm_hour = std::atoi(std::string(text.substr(11, 2)).c_str());
        tm.tm_min  = std::atoi(std::string(text.substr(14, 2)).c_str());
        tm.tm_sec  = std::atoi(std::string(text.substr(17, 2)).c_str());

        std::size_t i = 19;
        if (i < text.size() && text[i] == '.')
        {
          std::size_t j = i + 1;
          std::string digits;
          while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])))
            digits.push_back(text[j++]);
          digits.resize(6, '0');                       // to microseconds
          frac_us = std::atoll(digits.c_str());
          i = j;
        }
        if (i < text.size() && (text[i] == '+' || text[i] == '-') && i + 5 < text.size() + 1)
        {
          const int sign = text[i] == '-' ? -1 : 1;
          const int oh = std::atoi(std::string(text.substr(i + 1, 2)).c_str());
          const int om = std::atoi(std::string(text.substr(i + 4, 2)).c_str());
          offset_s = sign * (oh * 3600 + om * 60);
        }
      }
      const std::time_t utc = timegm(&tm);
      if (utc == static_cast<std::time_t>(-1)) return std::nullopt;
      return (static_cast<int64_t>(utc) - offset_s) * 1000000LL + frac_us;
    }

    // The timezone FhirProto records alongside the value. FHIR JSON carries an
    // OFFSET, not a zone name, and google/fhir accepts either -- round-tripping
    // the offset is what preserves the original spelling.
    inline std::string google_timezone_of(std::string_view text)
    {
      if (text.size() >= 20)
      {
        if (text.back() == 'Z') return "Z";
        const auto plus = text.find_last_of("+-");
        if (plus != std::string_view::npos && plus > 10)
          return std::string(text.substr(plus));
      }
      return "UTC";
    }

    // How many fractional-second digits the text actually carried. FhirProto
    // stores absolute microseconds, so PRECISION is the only record of how the
    // value was written -- stamping SECOND on everything turns
    // "06:06:32.842+00:00" into "06:06:32+00:00" on the way back out, which is
    // a different instant and a different string.
    inline int google_frac_digits(std::string_view text)
    {
      const auto dot = text.find('.');
      if (dot == std::string_view::npos) return 0;
      int n = 0;
      for (std::size_t i = dot + 1; i < text.size() &&
                                    std::isdigit(static_cast<unsigned char>(text[i])); ++i)
        ++n;
      return n;
    }

    template <typename PbDateTime>
    inline void google_set_datetime(std::string_view text, PbDateTime *pb)
    {
      if (pb == nullptr || text.empty()) return;
      const auto us = google_instant_us(text);
      if (!us) return;
      pb->set_value_us(*us);
      pb->set_timezone(google_timezone_of(text));
      const int frac = google_frac_digits(text);
      // Not every timelike type carries every precision: Date stops at DAY and
      // has no SECOND, Instant starts at SECOND and has no DAY. Probing keeps
      // one function for all of them and makes an impossible precision a
      // compile error rather than a silent mis-stamp.
      if constexpr (requires { PbDateTime::SECOND; })
      {
        if (text.size() <= 10)
        {
          if constexpr (requires { PbDateTime::DAY; }) pb->set_precision(PbDateTime::DAY);
          else                                         pb->set_precision(PbDateTime::SECOND);
        }
        else
          pb->set_precision(frac == 0 ? PbDateTime::SECOND
                            : frac <= 3 ? PbDateTime::MILLISECOND
                                        : PbDateTime::MICROSECOND);
      }
      else
        pb->set_precision(PbDateTime::DAY);
    }

    inline void google_set_instant(std::string_view text,
                                   google::fhir::r4::core::Instant *pb)
    {
      if (pb == nullptr || text.empty()) return;
      const auto us = google_instant_us(text);
      if (!us) return;
      using I = google::fhir::r4::core::Instant;
      pb->set_value_us(*us);
      pb->set_timezone(google_timezone_of(text));
      const int frac = google_frac_digits(text);
      pb->set_precision(frac == 0 ? I::SECOND : frac <= 3 ? I::MILLISECOND : I::MICROSECOND);
    }

    // EVERY coding, not just the first. `coding` is repeated in both FHIR and
    // the proto; copying `coding.front()` alone dropped the SNOMED half of every
    // dually-coded concept Synthea emits.
    inline void google_set_codeable_concept(const CodeableConceptData &src,
                                            google::fhir::r4::core::CodeableConcept *pb)
    {
      if (pb == nullptr) return;
      if (!src.text.empty()) pb->mutable_text()->set_value(std::string(src.text));
      for (const auto &c : src.coding)
      {
        auto *pc = pb->add_coding();
        if (!c.system.empty())  pc->mutable_system()->set_value(std::string(c.system));
        if (!c.code.empty())    pc->mutable_code()->set_value(std::string(c.code));
        if (!c.display.empty()) pc->mutable_display()->set_value(std::string(c.display));
      }
    }

    // A CodeableConcept rendered as JSON (the form a choice variant arrives in).
    inline void google_set_codeable_concept_json(const Json &v,
                                                 google::fhir::r4::core::CodeableConcept *pb)
    {
      if (pb == nullptr || !v.is_object()) return;
      if (v.contains("text") && v.at("text").is_string())
        pb->mutable_text()->set_value(v.at("text").get<std::string>());
      const auto codings = v.find("coding");
      if (codings == v.end() || !codings->is_array()) return;
      for (const auto &c : *codings)
      {
        if (!c.is_object()) continue;
        auto *pc = pb->add_coding();
        if (c.contains("system"))  pc->mutable_system()->set_value(c.at("system").get<std::string>());
        if (c.contains("code"))    pc->mutable_code()->set_value(c.at("code").get<std::string>());
        if (c.contains("display")) pc->mutable_display()->set_value(c.at("display").get<std::string>());
      }
    }

    // Quantity.value is a Decimal, and Decimal.value is a STRING -- FHIR keeps
    // the decimal's original lexical form (trailing zeros are significant), so
    // the proto does too. Writing it through a double would silently renormalise
    // "4.0" to "4".
    inline void google_set_quantity_json(const Json &v,
                                         google::fhir::r4::core::Quantity *pb)
    {
      if (pb == nullptr || !v.is_object()) return;
      if (v.contains("value"))
      {
        const Json &n = v.at("value");
        pb->mutable_value()->set_value(n.is_string() ? n.get<std::string>() : n.dump());
      }
      if (v.contains("unit"))   pb->mutable_unit()->set_value(v.at("unit").get<std::string>());
      if (v.contains("system")) pb->mutable_system()->set_value(v.at("system").get<std::string>());
      if (v.contains("code"))   pb->mutable_code()->set_value(v.at("code").get<std::string>());
    }

    // One choice value -> a `<Field>X` oneof. Routed through choice_block_json,
    // the SAME renderer the JSON and v2 arms use, so the three cannot disagree
    // about what the value is -- only about how their format stores it.
    template <typename PbValueX>
    inline void google_set_choice(const ChoiceEntry &choice, PbValueX *pb)
    {
      if (pb == nullptr || choice.is_empty()) return;

      // EACH `<Field>X` CARRIES ITS OWN ONEOF, and they are not the same set:
      // Observation.value[x] admits Quantity/CodeableConcept/string/boolean/...,
      // while Observation.effective[x] admits only dateTime/Period/Timing/instant.
      // Probing with `requires` lets one function serve both and makes an arm
      // that writes a variant its element does not allow a COMPILE error rather
      // than a silently dropped field.
      if (choice.block)
      {
        auto rendered = choice_block_json(*choice.block);
        if (!rendered) return;
        const Json &v = rendered->value;
        if constexpr (requires { pb->mutable_quantity(); })
          if (rendered->suffix == "Quantity") { google_set_quantity_json(v, pb->mutable_quantity()); return; }
        if constexpr (requires { pb->mutable_codeable_concept(); })
          if (rendered->suffix == "CodeableConcept") { google_set_codeable_concept_json(v, pb->mutable_codeable_concept()); return; }
        // Range/Ratio/SampledData/Period are in the oneof but absent from this
        // corpus; unset is visible as a missing leaf, never as a wrong one.
        return;
      }

      if constexpr (requires { pb->mutable_date_time(); })
        if (FF_IsDateTimeTag(choice.tag))
        {
          // MultipleBirthX admits only boolean and integer -- no date/time
          // member exists to write to, so the probe skips this arm entirely.
          const std::string text = choice.to_string();
          if (!text.empty()) google_set_datetime(text, pb->mutable_date_time());
          return;
        }
      if constexpr (requires { pb->mutable_boolean(); })
        if (std::holds_alternative<bool>(choice.value))
        { pb->mutable_boolean()->set_value(std::get<bool>(choice.value)); return; }
      if constexpr (requires { pb->mutable_integer(); })
        if (std::holds_alternative<int32_t>(choice.value))
        { pb->mutable_integer()->set_value(std::get<int32_t>(choice.value)); return; }
      if constexpr (requires { pb->mutable_quantity(); })
        if (std::holds_alternative<double>(choice.value))
        { pb->mutable_quantity()->mutable_value()->set_value(Json(std::get<double>(choice.value)).dump()); return; }
      if constexpr (requires { pb->mutable_string_value(); })
        if (std::holds_alternative<std::string_view>(choice.value))
          pb->mutable_string_value()->set_value(std::string(std::get<std::string_view>(choice.value)));
    }

    inline void google_set_address_json(const Json &v, google::fhir::r4::core::Address *pb)
    {
      if (pb == nullptr || !v.is_object()) return;
      if (v.contains("text"))       pb->mutable_text()->set_value(v.at("text").get<std::string>());
      if (v.contains("city"))       pb->mutable_city()->set_value(v.at("city").get<std::string>());
      if (v.contains("state"))      pb->mutable_state()->set_value(v.at("state").get<std::string>());
      if (v.contains("postalCode")) pb->mutable_postal_code()->set_value(v.at("postalCode").get<std::string>());
      if (v.contains("country"))    pb->mutable_country()->set_value(v.at("country").get<std::string>());
      const auto lines = v.find("line");
      if (lines != v.end() && lines->is_array())
        for (const auto &l : *lines)
          if (l.is_string()) pb->add_line()->set_value(l.get<std::string>());
    }

    inline void google_set_coding_json(const Json &v, google::fhir::r4::core::Coding *pb)
    {
      if (pb == nullptr || !v.is_object()) return;
      if (v.contains("system"))  pb->mutable_system()->set_value(v.at("system").get<std::string>());
      if (v.contains("code"))    pb->mutable_code()->set_value(v.at("code").get<std::string>());
      if (v.contains("display")) pb->mutable_display()->set_value(v.at("display").get<std::string>());
    }

    inline void google_build_extension(const ExtensionData &src, google::fhir::r4::core::Extension *pb_ext)
    {
      if (!src.id.empty())
        pb_ext->mutable_id()->set_value(std::string(src.id));

      // The REAL url, resolved from FastFHIR's trie -- src.url is an INDEX into
      // the URL directory, not a URL. Fabricating one meant the arm never
      // encoded the document's actual extension URLs. An unresolvable index
      // writes nothing rather than a plausible-looking lie.
      if (src.url != FF_NULL_UINT32)
        if (const std::string __u = resolve_extension_url(src.url); !__u.empty())
          pb_ext->mutable_url()->set_value(__u);

      if (!src.value.is_empty())
      {
        auto *pb_val = pb_ext->mutable_value();

        // A BLOCK-TYPED VARIANT. Extension.value[x] admits the whole datatype
        // list, and only the four inline scalars below were ever handled, so
        // every Coding-, Address- and CodeableConcept-valued extension in the
        // corpus was silently dropped -- including the US Core race and
        // ethnicity extensions, whose payload is entirely nested Codings.
        // Routed through choice_block_json, the same renderer the other arms
        // use, so the arms cannot disagree about what the value is.
        if (src.value.block)
        {
          if (auto rendered = choice_block_json(*src.value.block))
          {
            const Json &v = rendered->value;
            const std::string &t = rendered->suffix;
            if (t == "Coding")               google_set_coding_json(v, pb_val->mutable_coding());
            else if (t == "Address")         google_set_address_json(v, pb_val->mutable_address());
            else if (t == "CodeableConcept") google_set_codeable_concept_json(v, pb_val->mutable_codeable_concept());
            else if (t == "Quantity")        google_set_quantity_json(v, pb_val->mutable_quantity());
          }
        }
        else if (FF_IsDateTimeTag(src.value.tag))
        {
          const std::string text = src.value.to_string();
          if (!text.empty()) google_set_datetime(text, pb_val->mutable_date_time());
        }
        else if (std::holds_alternative<std::string_view>(src.value.value))
        {
          // A `code` is not a `string`. Both arrive in the string_view
          // alternative, and only the tag distinguishes them -- writing every
          // one to string_value turned each valueCode into a valueString,
          // which is a different element name in the document.
          const std::string text(std::get<std::string_view>(src.value.value));
          if (src.value.tag == RECOVER_FF_CODE) pb_val->mutable_code()->set_value(text);
          else                                  pb_val->mutable_string_value()->set_value(text);
        }
        else if (std::holds_alternative<bool>(src.value.value))
          pb_val->mutable_boolean()->set_value(std::get<bool>(src.value.value));
        else if (std::holds_alternative<int32_t>(src.value.value))
          pb_val->mutable_integer()->set_value(std::get<int32_t>(src.value.value));
        else if (std::holds_alternative<double>(src.value.value))
          // FHIR decimal keeps its lexical form, so the proto stores a string.
          pb_val->mutable_decimal()->set_value(std::to_string(std::get<double>(src.value.value)));
      }

      for (const auto &nested : src.extension)
        google_build_extension(nested, pb_ext->add_extension());
    }

#endif  // ARM_GOOGLE_FHIR



    inline void write_choice(Json &out, const std::string_view base, const ChoiceEntry &choice)
    {
      if (choice.is_empty())
      {
        return;
      }
      // A block-typed variant lives in `.block`; `.value` is monostate for it,
      // so the visitor below would emit nothing at all. Its key carries the
      // DATATYPE's name, taken from the variant rather than from the tag.
      if (choice.block)
      {
        if (auto rendered = choice_block_json(*choice.block))
        {
          out[std::string(base) + rendered->suffix] = std::move(rendered->value);
        }
        return;
      }

      const auto key = std::string(base) + choice_suffix(choice.tag);

      // A DATE/TIME VARIANT IS TEXT, NOT ITS PACKED INTEGER. The four
      // date/time tags store a packed civil value in an int64 slot, so the
      // alternative below is genuinely an integer and would be dumped as
      // 1619552459707908099 where every other reader of this document -- the
      // neutral POCO walker, the JSON arm's own effective[x] path, FastFHIR's
      // print_json -- renders "2021-04-27T18:20:59+00:00". to_string() is
      // FastFHIR's own renderer, the same call poco_leaves.hpp makes, so the
      // projection cannot drift from it.
      if (FF_IsDateTimeTag(choice.tag))
      {
        const std::string text = choice.to_string();
        if (!text.empty())
          out[key] = text;
        return;
      }

      if (std::holds_alternative<bool>(choice.value))
      {
        out[key] = std::get<bool>(choice.value);
      }
      else if (std::holds_alternative<int32_t>(choice.value))
      {
        out[key] = std::get<int32_t>(choice.value);
      }
      else if (std::holds_alternative<uint32_t>(choice.value))
      {
        out[key] = std::get<uint32_t>(choice.value);
      }
      else if (std::holds_alternative<int64_t>(choice.value))
      {
        out[key] = std::get<int64_t>(choice.value);
      }
      else if (std::holds_alternative<uint64_t>(choice.value))
      {
        out[key] = std::get<uint64_t>(choice.value);
      }
      else if (std::holds_alternative<double>(choice.value))
      {
        out[key] = std::get<double>(choice.value);
      }
      else if (std::holds_alternative<std::string_view>(choice.value))
      {
        out[key] = std::string(std::get<std::string_view>(choice.value));
      }
    }

    inline void put_if_string(Json &out, const char *key, std::string_view value)
    {
      if (!value.empty())
        out[key] = value;
    }
    inline void put_if_u32(Json &out, const char *key, uint32_t value)
    {
      if (has_u32(value))
        out[key] = value;
    }
    inline void put_if_f64(Json &out, const char *key, double value)
    {
      if (has_f64(value))
        out[key] = value;
    }
    inline void put_if_bool_flag(Json &out, const char *key, uint8_t flag)
    {
      if (has_u8(flag))
        out[key] = (flag != 0);
    }
    inline void put_if_enum(Json &out, const char *key, int value)
    {
      // FF_NULL_UINT8 is the ABSENT sentinel for these uint8_t enum fields, and
      // testing only `!= 0` let it through as a literal 255 -- so a Quantity
      // carried {"comparator": 255}, which is an integer where FHIR requires a
      // code, and 1,521 such leaves across the corpus. They were invisible
      // while block-typed choices were dropped entirely; rendering the Quantity
      // is what surfaced them.
      //
      // No FHIR code enum has 255 ordinals, and has_u8() above already treats
      // that value as absence.
      //
      // ZERO IS A VALUE, NOT AN ABSENCE. FastFHIR generates these enums
      // ALPHABETICALLY, so ordinal 0 is a real code in every one of them --
      // FF_AdministrativeGender::Female, FF_ContactPointUse::Home,
      // FF_ContactPointSystem::Email, FF_NameUse::Anonymous. Excluding it
      // dropped the first code of every ValueSet: three of five patients had
      // no gender and every telecom lost its `use`. The sentinel is 255 and
      // only 255; that is the whole test.
      if (value != static_cast<int>(FF_NULL_UINT8))
        out[key] = value;
    }

    // ------- Datatypes -------

    inline Json to_json_extension(const ExtensionData &src);
    inline Json to_json_coding(const CodingData &src);
    inline Json to_json_codeable_concept(const CodeableConceptData &src);
    inline Json to_json_period(const PeriodData &src);
    inline Json to_json_quantity(const QuantityData &src);
    inline Json to_json_reference(const ReferenceData &src);
    inline Json to_json_identifier(const IdentifierData &src);
    inline Json to_json_meta(const MetaData &src);
    inline Json to_json_narrative(const NarrativeData &src);
    inline Json to_json_human_name(const HumanNameData &src);
    inline Json to_json_address(const AddressData &src);
    inline Json to_json_contact_point(const ContactPointData &src);
    inline Json to_json_attachment(const AttachmentData &src);
    inline Json to_json_range(const RangeData &src);
    inline Json to_json_annotation(const AnnotationData &src);
    inline Json to_json_observation_triggered_by(const ObservationtriggeredByData &src);
    inline Json to_json_observation_reference_range(const ObservationreferenceRangeData &src);
    inline Json to_json_observation_component(const ObservationcomponentData &src);

    // ── Rendering a DECODED block-typed choice ──────────────────────────────
    //
    // ChoiceEntry used to hand out a raw source-arena OFFSET for a block-typed
    // value[x], so every arm either wrote it out as a bare number
    // ({"valueQuantity": 55683}) or, once that was noticed, dropped the field
    // outright. FastFHIR now carries the DECODED value in `.block`, so the
    // measurement it always should have been is finally available: the arms can
    // render the Quantity.
    //
    // One dispatch for every arm. The overloads below forward to the existing
    // per-datatype converters, and `requires` lets a datatype this build has no
    // converter for degrade EXPLICITLY -- counted, not silently skipped, which
    // is how the offset survived in the first place.
    inline Json to_json_choice_value(const AddressData &v) { return to_json_address(v); }
    inline Json to_json_choice_value(const AnnotationData &v) { return to_json_annotation(v); }
    inline Json to_json_choice_value(const AttachmentData &v) { return to_json_attachment(v); }
    inline Json to_json_choice_value(const CodeableConceptData &v) { return to_json_codeable_concept(v); }
    inline Json to_json_choice_value(const CodingData &v) { return to_json_coding(v); }
    inline Json to_json_choice_value(const ContactPointData &v) { return to_json_contact_point(v); }
    inline Json to_json_choice_value(const ExtensionData &v) { return to_json_extension(v); }
    inline Json to_json_choice_value(const HumanNameData &v) { return to_json_human_name(v); }
    inline Json to_json_choice_value(const IdentifierData &v) { return to_json_identifier(v); }
    inline Json to_json_choice_value(const MetaData &v) { return to_json_meta(v); }
    inline Json to_json_choice_value(const NarrativeData &v) { return to_json_narrative(v); }
    inline Json to_json_choice_value(const PeriodData &v) { return to_json_period(v); }
    inline Json to_json_choice_value(const QuantityData &v) { return to_json_quantity(v); }
    inline Json to_json_choice_value(const RangeData &v) { return to_json_range(v); }
    inline Json to_json_choice_value(const ReferenceData &v) { return to_json_reference(v); }

    // Datatypes with no converter yet. Counted so a gap shows up as a number
    // rather than as a field that quietly is not there; ChoiceBlock has 29
    // members and 15 are covered today.
    inline std::size_t &unrendered_choice_count() {
        static std::size_t n = 0;
        return n;
    }

    // THE SUFFIX COMES FROM THE VARIANT, NOT FROM THE TAG.
    //
    // choice_suffix() defaults to "String" for any tag it does not list, and it
    // lists only the scalars -- so a Quantity rendered as {"valueString": {...}}:
    // an object under a primitive's key, invalid FHIR, and silent. Reading the
    // name off the alternative that is actually stored cannot default wrong;
    // a datatype with no overload here fails to compile rather than mislabel.
    inline const char *choice_suffix_for(const AddressData &) { return "Address"; }
    inline const char *choice_suffix_for(const AnnotationData &) { return "Annotation"; }
    inline const char *choice_suffix_for(const AttachmentData &) { return "Attachment"; }
    inline const char *choice_suffix_for(const CodeableConceptData &) { return "CodeableConcept"; }
    inline const char *choice_suffix_for(const CodingData &) { return "Coding"; }
    inline const char *choice_suffix_for(const ContactPointData &) { return "ContactPoint"; }
    inline const char *choice_suffix_for(const ExtensionData &) { return "Extension"; }
    inline const char *choice_suffix_for(const HumanNameData &) { return "HumanName"; }
    inline const char *choice_suffix_for(const IdentifierData &) { return "Identifier"; }
    inline const char *choice_suffix_for(const MetaData &) { return "Meta"; }
    inline const char *choice_suffix_for(const NarrativeData &) { return "Narrative"; }
    inline const char *choice_suffix_for(const PeriodData &) { return "Period"; }
    inline const char *choice_suffix_for(const QuantityData &) { return "Quantity"; }
    inline const char *choice_suffix_for(const RangeData &) { return "Range"; }
    inline const char *choice_suffix_for(const ReferenceData &) { return "Reference"; }

    inline std::optional<RenderedChoice> choice_block_json(const ChoiceBlock &blk) {
        std::optional<RenderedChoice> out;
        std::visit(
            [&](const auto &v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    // An empty variant is not a gap: nothing was stored.
                } else if constexpr (requires { to_json_choice_value(v); }) {
                    out = RenderedChoice{choice_suffix_for(v), to_json_choice_value(v)};
                } else {
                    ++unrendered_choice_count();
                }
            },
            blk.value);
        return out;
    }

    inline Json to_json_extension(const ExtensionData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      // FF_NULL_UINT32 is "absent"; 0 is a legitimate intern index.
      //
      // Emit the URL, not the index. `urlIndex` is a FastFHIR storage detail
      // (the radix trie that stores each path segment once); FHIR requires the
      // string, and a JSON arm that writes the index is not producing FHIR.
      // An index the table cannot resolve is a real gap -- omit it rather than
      // write a non-FHIR key, and let the corpus round-trip check report it.
      if (src.url != FF_NULL_UINT32) {
        const std::string url = resolve_extension_url(src.url);
        if (!url.empty())
          out["url"] = url;
      }
      write_choice(out, "value", src.value);
      return out;
    }

    inline Json to_json_coding(const CodingData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      put_if_string(out, "system", src.system);
      put_if_string(out, "version", src.version);
      put_if_string(out, "code", src.code);
      put_if_string(out, "display", src.display);
      put_if_bool_flag(out, "userSelected", src.userselected);
      return out;
    }

    inline Json to_json_codeable_concept(const CodeableConceptData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_string(out, "text", src.text);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (!src.coding.empty())
      {
        out["coding"] = Json::array();
        for (const auto &c : src.coding)
          out["coding"].push_back(to_json_coding(c));
      }
      return out;
    }

    inline Json to_json_period(const PeriodData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_string(out, "start", src.start);
      put_if_string(out, "end", src.end);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      return out;
    }

    inline Json to_json_quantity(const QuantityData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_f64(out, "value", src.value);
      put_if_enum(out, "comparator", static_cast<int>(src.comparator));
      put_if_string(out, "unit", src.unit);
      put_if_string(out, "system", src.system);
      put_if_string(out, "code", src.code);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      return out;
    }

    inline Json to_json_reference(const ReferenceData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_string(out, "reference", src.reference);
      put_if_string(out, "type", src.type);
      put_if_string(out, "display", src.display);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (src.identifier)
        out["identifier"] = to_json_identifier(*src.identifier);
      return out;
    }

    inline Json to_json_identifier(const IdentifierData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_enum(out, "use", static_cast<int>(src.use));
      put_if_string(out, "system", src.system);
      put_if_string(out, "value", src.value);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (src.type)
        out["type"] = to_json_codeable_concept(*src.type);
      if (src.period)
        out["period"] = to_json_period(*src.period);
      if (src.assigner)
        out["assigner"] = to_json_reference(*src.assigner);
      return out;
    }

    inline Json to_json_meta(const MetaData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_string(out, "versionId", src.versionid);
      put_if_string(out, "lastUpdated", src.lastupdated);
      put_if_string(out, "source", src.source);
      if (!src.profile.empty())
      {
        out["profile"] = Json::array();
        for (auto v : src.profile)
          if (!v.empty())
            out["profile"].push_back(v);
      }
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (!src.security.empty())
      {
        out["security"] = Json::array();
        for (const auto &c : src.security)
          out["security"].push_back(to_json_coding(c));
      }
      if (!src.tag.empty())
      {
        out["tag"] = Json::array();
        for (const auto &c : src.tag)
          out["tag"].push_back(to_json_coding(c));
      }
      return out;
    }

    inline Json to_json_narrative(const NarrativeData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_enum(out, "status", static_cast<int>(src.status));
      put_if_string(out, "div", src.div);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      return out;
    }

    inline Json to_json_human_name(const HumanNameData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_enum(out, "use", static_cast<int>(src.use));
      put_if_string(out, "text", src.text);
      put_if_string(out, "family", src.family);
      if (!src.given.empty())
      {
        out["given"] = Json::array();
        for (auto v : src.given)
          if (!v.empty())
            out["given"].push_back(v);
      }
      if (!src.prefix.empty())
      {
        out["prefix"] = Json::array();
        for (auto v : src.prefix)
          if (!v.empty())
            out["prefix"].push_back(v);
      }
      if (!src.suffix.empty())
      {
        out["suffix"] = Json::array();
        for (auto v : src.suffix)
          if (!v.empty())
            out["suffix"].push_back(v);
      }
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (src.period)
        out["period"] = to_json_period(*src.period);
      return out;
    }

    inline Json to_json_address(const AddressData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_enum(out, "use", static_cast<int>(src.use));
      put_if_enum(out, "type", static_cast<int>(src.type));
      put_if_string(out, "text", src.text);
      if (!src.line.empty())
      {
        out["line"] = Json::array();
        for (auto v : src.line)
          if (!v.empty())
            out["line"].push_back(v);
      }
      put_if_string(out, "city", src.city);
      put_if_string(out, "district", src.district);
      put_if_string(out, "state", src.state);
      put_if_string(out, "postalCode", src.postalcode);
      put_if_string(out, "country", src.country);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (src.period)
        out["period"] = to_json_period(*src.period);
      return out;
    }

    inline Json to_json_contact_point(const ContactPointData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_enum(out, "system", static_cast<int>(src.system));
      put_if_string(out, "value", src.value);
      put_if_enum(out, "use", static_cast<int>(src.use));
      put_if_u32(out, "rank", src.rank);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (src.period)
        out["period"] = to_json_period(*src.period);
      return out;
    }

    inline Json to_json_attachment(const AttachmentData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_string(out, "contentType", src.contenttype);
      put_if_string(out, "language", src.language);
      // PORT-6: Attachment.data / .hash are std::unique_ptr<std::string_view>
      // upstream -- the base64 payload that compaction used to drop (CMP-1).
      if (src.data)
        put_if_string(out, "data", *src.data);
      put_if_string(out, "url", src.url);
      put_if_u32(out, "size", src.size);
      if (src.hash)
        put_if_string(out, "hash", *src.hash);
      put_if_string(out, "title", src.title);
      put_if_string(out, "creation", src.creation);
      put_if_u32(out, "height", src.height);
      put_if_u32(out, "width", src.width);
      put_if_u32(out, "frames", src.frames);
      put_if_f64(out, "duration", src.duration);
      put_if_u32(out, "pages", src.pages);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      return out;
    }

    inline Json to_json_range(const RangeData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (src.low)
        out["low"] = to_json_quantity(*src.low);
      if (src.high)
        out["high"] = to_json_quantity(*src.high);
      return out;
    }

    inline Json to_json_annotation(const AnnotationData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      write_choice(out, "author", src.author);
      put_if_string(out, "time", src.time);
      put_if_string(out, "text", src.text);
      return out;
    }

    inline Json to_json_observation_triggered_by(const ObservationtriggeredByData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (!src.modifierextension.empty())
      {
        out["modifierExtension"] = Json::array();
        for (const auto &e : src.modifierextension)
          out["modifierExtension"].push_back(to_json_extension(e));
      }
      if (src.observation)
        out["observation"] = to_json_reference(*src.observation);
      put_if_enum(out, "type", static_cast<int>(src.type));
      put_if_string(out, "reason", src.reason);
      return out;
    }

    inline Json to_json_observation_reference_range(const ObservationreferenceRangeData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (!src.modifierextension.empty())
      {
        out["modifierExtension"] = Json::array();
        for (const auto &e : src.modifierextension)
          out["modifierExtension"].push_back(to_json_extension(e));
      }
      if (src.low)
        out["low"] = to_json_quantity(*src.low);
      if (src.high)
        out["high"] = to_json_quantity(*src.high);
      if (src.type)
        out["type"] = to_json_codeable_concept(*src.type);
      if (!src.appliesto.empty())
      {
        out["appliesTo"] = Json::array();
        for (const auto &c : src.appliesto)
          out["appliesTo"].push_back(to_json_codeable_concept(c));
      }
      if (src.age)
        out["age"] = to_json_range(*src.age);
      put_if_string(out, "text", src.text);
      if (src.normalvalue)
        out["normalValue"] = to_json_codeable_concept(*src.normalvalue);
      return out;
    }

    inline Json to_json_observation_component(const ObservationcomponentData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (!src.modifierextension.empty())
      {
        out["modifierExtension"] = Json::array();
        for (const auto &e : src.modifierextension)
          out["modifierExtension"].push_back(to_json_extension(e));
      }
      if (src.code)
        out["code"] = to_json_codeable_concept(*src.code);
      write_choice(out, "value", src.value);
      if (src.dataabsentreason)
        out["dataAbsentReason"] = to_json_codeable_concept(*src.dataabsentreason);
      if (!src.interpretation.empty())
      {
        out["interpretation"] = Json::array();
        for (const auto &c : src.interpretation)
          out["interpretation"].push_back(to_json_codeable_concept(c));
      }
      if (!src.referencerange.empty())
      {
        out["referenceRange"] = Json::array();
        for (const auto &rr : src.referencerange)
          out["referenceRange"].push_back(to_json_observation_reference_range(rr));
      }
      return out;
    }

    // ------- Patient nested -------
    inline Json to_json_patient_contact(const PatientcontactData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_enum(out, "gender", static_cast<int>(src.gender));
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (!src.modifierextension.empty())
      {
        out["modifierExtension"] = Json::array();
        for (const auto &e : src.modifierextension)
          out["modifierExtension"].push_back(to_json_extension(e));
      }
      if (!src.relationship.empty())
      {
        out["relationship"] = Json::array();
        for (const auto &c : src.relationship)
          out["relationship"].push_back(to_json_codeable_concept(c));
      }
      if (src.name)
        out["name"] = to_json_human_name(*src.name);
      if (!src.telecom.empty())
      {
        out["telecom"] = Json::array();
        for (const auto &t : src.telecom)
          out["telecom"].push_back(to_json_contact_point(t));
      }
      if (src.address)
        out["address"] = to_json_address(*src.address);
      if (src.organization)
        out["organization"] = to_json_reference(*src.organization);
      if (src.period)
        out["period"] = to_json_period(*src.period);
      return out;
    }

    inline Json to_json_patient_communication(const PatientcommunicationData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_bool_flag(out, "preferred", src.preferred);
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (!src.modifierextension.empty())
      {
        out["modifierExtension"] = Json::array();
        for (const auto &e : src.modifierextension)
          out["modifierExtension"].push_back(to_json_extension(e));
      }
      if (src.language)
        out["language"] = to_json_codeable_concept(*src.language);
      return out;
    }

    inline Json to_json_patient_link(const PatientlinkData &src)
    {
      Json out = Json::object();
      put_if_string(out, "id", src.id);
      put_if_enum(out, "type", static_cast<int>(src.type));
      if (!src.extension.empty())
      {
        out["extension"] = Json::array();
        for (const auto &e : src.extension)
          out["extension"].push_back(to_json_extension(e));
      }
      if (!src.modifierextension.empty())
      {
        out["modifierExtension"] = Json::array();
        for (const auto &e : src.modifierextension)
          out["modifierExtension"].push_back(to_json_extension(e));
      }
      if (src.other)
        out["other"] = to_json_reference(*src.other);
      return out;
    }

#if defined(ARM_HL7V2)
    inline Json hl7_json_value(std::string_view src) { return std::string(src); }
    inline Json hl7_json_value(const std::string &src) { return src; }
    inline Json hl7_json_value(bool src) { return src; }
    inline Json hl7_json_value(uint8_t src) { return src != 0; }
    inline Json hl7_json_value(uint32_t src) { return src; }
    inline Json hl7_json_value(int src) { return src; }
    inline Json hl7_json_value(double src) { return src; }
    inline Json hl7_json_value(const ResourceReference &src)
    {
      return Json{{"offset", src.offset}, {"recovery", static_cast<int>(src.recovery)}};
    }
    inline Json hl7_json_value(const ExtensionData &src) { return to_json_extension(src); }
    inline Json hl7_json_value(const CodingData &src) { return to_json_coding(src); }
    inline Json hl7_json_value(const CodeableConceptData &src) { return to_json_codeable_concept(src); }
    inline Json hl7_json_value(const PeriodData &src) { return to_json_period(src); }
    inline Json hl7_json_value(const QuantityData &src) { return to_json_quantity(src); }
    inline Json hl7_json_value(const ReferenceData &src) { return to_json_reference(src); }
    inline Json hl7_json_value(const IdentifierData &src) { return to_json_identifier(src); }
    inline Json hl7_json_value(const MetaData &src) { return to_json_meta(src); }
    inline Json hl7_json_value(const NarrativeData &src) { return to_json_narrative(src); }
    inline Json hl7_json_value(const HumanNameData &src) { return to_json_human_name(src); }
    inline Json hl7_json_value(const AddressData &src) { return to_json_address(src); }
    inline Json hl7_json_value(const ContactPointData &src) { return to_json_contact_point(src); }
    inline Json hl7_json_value(const AttachmentData &src) { return to_json_attachment(src); }
    inline Json hl7_json_value(const RangeData &src) { return to_json_range(src); }
    inline Json hl7_json_value(const AnnotationData &src) { return to_json_annotation(src); }
    inline Json hl7_json_value(const ObservationtriggeredByData &src)
    {
      return to_json_observation_triggered_by(src);
    }
    inline Json hl7_json_value(const ObservationreferenceRangeData &src)
    {
      return to_json_observation_reference_range(src);
    }
    inline Json hl7_json_value(const ObservationcomponentData &src)
    {
      return to_json_observation_component(src);
    }
    inline Json hl7_json_value(const PatientcontactData &src) { return to_json_patient_contact(src); }
    inline Json hl7_json_value(const PatientcommunicationData &src)
    {
      return to_json_patient_communication(src);
    }
    inline Json hl7_json_value(const PatientlinkData &src) { return to_json_patient_link(src); }
    inline Json hl7_json_value(const ChoiceEntry &choice)
    {
      Json out = Json::object();
      write_choice(out, "value", choice);
      return out;
    }

    template <typename T>
    inline Json hl7_json_array(const std::vector<T> &values)
    {
      Json out = Json::array();
      for (const auto &value : values)
      {
        out.push_back(hl7_json_value(value));
      }
      return out;
    }

    template <typename Target>
    inline void hl7_append_json_field(Target &dst, const char *field_name, const Json &payload)
    {
      dst.append_custom_field(field_name, payload.dump());
    }

    template <typename Target>
    inline void hl7_mark_if_string(Target &dst, const char *field_name, std::string_view value)
    {
      if (!value.empty())
      {
        hl7_append_json_field(dst, field_name, hl7_json_value(value));
      }
    }

    template <typename Target>
    inline void hl7_mark_if_bool(Target &dst, const char *field_name, uint8_t value)
    {
      if (has_u8(value))
      {
        hl7_append_json_field(dst, field_name, hl7_json_value(value));
      }
    }

    template <typename Target, typename Pointer>
    inline void hl7_mark_if_pointer(Target &dst, const char *field_name, const Pointer &value)
    {
      if (value)
      {
        hl7_append_json_field(dst, field_name, hl7_json_value(*value));
      }
    }

    template <typename Target, typename T>
    inline void hl7_mark_if_vector(Target &dst, const char *field_name, const std::vector<T> &values)
    {
      if (!values.empty())
      {
        hl7_append_json_field(dst, field_name, hl7_json_array(values));
      }
    }

    // "observation.effective[x]" -> "effective". The ZFX name carries the
    // FHIR element path, so the base of a choice is recoverable from it and
    // does not need passing separately.
    inline std::string hl7_choice_base(const char *field_name)
    {
      std::string fn(field_name);
      if (fn.size() > 3 && fn.compare(fn.size() - 3, 3, "[x]") == 0)
        fn.resize(fn.size() - 3);
      const auto dot = fn.rfind('.');
      return dot == std::string::npos ? fn : fn.substr(dot + 1);
    }

    template <typename Target>
    inline void hl7_mark_if_choice(Target &dst, const char *field_name, const ChoiceEntry &choice)
    {
      if (choice.is_empty())
        return;
      // The payload is keyed by the FULL FHIR element name -- `effectiveDateTime`,
      // not `valueDateTime`. It used to be built by hl7_json_value(ChoiceEntry),
      // which hardcodes the base "value" for EVERY choice field, so every
      // effective[x] on the wire announced itself as a value[x] and no decoder
      // recombining the ZFX name with the payload key could agree with the
      // other arms' canonical paths.
      //
      // A block-typed variant is in `.block`; the ZFX passthrough carries JSON,
      // so it renders through the same dispatch the JSON arm uses -- the arms
      // must agree on what the value IS, or the comparison measures the
      // encoders' disagreement rather than the formats'.
      const std::string base = hl7_choice_base(field_name);
      Json out = Json::object();
      if (choice.block)
      {
        auto rendered = choice_block_json(*choice.block);
        if (!rendered)
          return;
        // Wrapped, like the scalar arm below. Emitting the bare value here and
        // an object there made the payload shape depend on the variant.
        out[base + rendered->suffix] = std::move(rendered->value);
      }
      else
      {
        write_choice(out, base, choice);
      }
      hl7_append_json_field(dst, field_name, out);
    }
#endif

    // ------- Unified Patient field assignment (JSON + FFHR stream) -------

#if defined(ARM_FASTFHIR)
    struct PatientStreamSink
    {
      FastFHIR::Builder &builder;
      FastFHIR::Reflective::ObjectHandle handle;
    };

    // PORT-7 (2026-08-25): this used to hand-roll the custom-CODE wire encoding
    // -- append a bare FF_STRING, compute an offset relative to the SLOT, OR in
    // FF_CUSTOM_STRING_FLAG. Every part of that is now wrong:
    //
    //   * FF_CUSTOM_STRING_FLAG no longer exists.
    //   * The discriminator moved from bit 30 to bit 31 (FF_CODEABLE_CONCEPT_FLAG),
    //     reclaiming bit 30 for payload.
    //   * With the flag set the offset now targets an FF_CODEABLE_CONCEPT block,
    //     not a bare FF_STRING, and is relative to the CONTAINING BLOCK rather
    //     than to the slot.
    //
    // Substituting the new constant would have emitted a valid-looking slot
    // pointing at the wrong block type -- wrong bytes, no compile error. So the
    // hand-rolled path is gone; ENCODE_FF_CODE is the public encoder and owns
    // both branches (dictionary hit -> 31-bit index with MSB clear; miss ->
    // packed relative offset with MSB set). See notes.md.
    inline void stream_assign_code_field(PatientStreamSink &dst, FF_FieldKey key, std::string_view code_value)
    {
      const Offset block_offset = dst.handle.offset();

      if (code_value.empty())
      {
        dst.builder.amend_scalar<uint32_t>(block_offset, key.field_offset, FF_CODE_NULL);
        return;
      }

      const std::string code_str(code_value);
      const uint32_t version = dst.builder.FhirVersion();

      // Fast path: permanent-dictionary hit needs no child space at all.
      uint32_t slot = FF_GetDictionaryCode(code_str, version);
      if (slot == FF_CODE_NULL)
      {
        // Miss: ENCODE_FF_CODE writes an FF_CODEABLE_CONCEPT into child space
        // and advances the cursor, so claim exactly SIZE_FF_CODE bytes first.
        const Size child_size = SIZE_FF_CODE(code_value, version);
        Offset child_off =
            FastFHIR::AdvancedBuilderAccess(dst.builder).UNSAFE_allocate_raw(child_size);
        if (child_off == FF_NULL_OFFSET)
        {
          throw std::runtime_error(
              "FastFHIR benchmark assignment: arena claim failed for custom CODE '" + code_str + "'");
        }

        const Offset child_begin = child_off;
        slot = ENCODE_FF_CODE(dst.builder.memory().base(), block_offset, child_off,
                                        code_str, version,
                                        FF_CodeableConceptSystem::UNKNOWN);

        // The SIZE/ENCODE contract is the same one Builder::append enforces for
        // SIZE/STORE: a disagreement means the next claim overlaps this block.
        if (child_off != child_begin + child_size)
        {
          throw std::runtime_error(
              "FastFHIR benchmark assignment: SIZE_FF_CODE/ENCODE_FF_CODE disagree for '" +
              code_str + "' (claimed " + std::to_string(child_size) + ", wrote " +
              std::to_string(child_off - child_begin) + ")");
        }
      }
      dst.builder.amend_scalar<uint32_t>(block_offset, key.field_offset, slot);
    }

    inline void stream_assign_choice_field(PatientStreamSink &dst, FF_FieldKey key, const ChoiceEntry &choice,
                                           const char *field_name)
    {
      if (choice.is_empty())
        return;
      auto assign_raw_variant = [&](uint64_t raw_bits)
      {
        dst.builder.amend_variant(dst.handle.offset(), key.field_offset, raw_bits, choice.tag);
      };

      // REBUILT, NOT REPOINTED. The decoded value is appended into THIS
      // builder's arena and the slot names the address it got here. Writing the
      // source arena's offset is what used to corrupt this arm's own stream
      // ("the offset chain is broken") and is the reason the field was dropped.
      if (choice.block)
      {
        std::visit(
            [&](const auto &v)
            {
              using T = std::decay_t<decltype(v)>;
              if constexpr (!std::is_same_v<T, std::monostate>)
              {
                assign_raw_variant(
                    static_cast<uint64_t>(dst.builder.append_obj(v).offset()));
              }
            },
            choice.block->value);
        return;
      }

      if (std::holds_alternative<bool>(choice.value))
      {
        assign_raw_variant(static_cast<uint64_t>(std::get<bool>(choice.value) ? 1 : 0));
      }
      else if (std::holds_alternative<int32_t>(choice.value))
      {
        assign_raw_variant(static_cast<uint64_t>(std::get<int32_t>(choice.value)));
      }
      else if (std::holds_alternative<uint32_t>(choice.value))
      {
        assign_raw_variant(static_cast<uint64_t>(std::get<uint32_t>(choice.value)));
      }
      else if (std::holds_alternative<int64_t>(choice.value))
      {
        assign_raw_variant(static_cast<uint64_t>(std::get<int64_t>(choice.value)));
      }
      else if (std::holds_alternative<uint64_t>(choice.value))
      {
        assign_raw_variant(std::get<uint64_t>(choice.value));
      }
      else if (std::holds_alternative<double>(choice.value))
      {
        const double val = std::get<double>(choice.value);
        uint64_t raw = 0;
        std::memcpy(&raw, &val, sizeof(double));
        assign_raw_variant(raw);
      }
      else if (std::holds_alternative<std::string_view>(choice.value))
      {
        const auto s = std::get<std::string_view>(choice.value);
        const auto off = dst.builder.append(s);
        assign_raw_variant(off);
      }
      else
      {
        throw std::runtime_error(std::string("FastFHIR benchmark assignment: unsupported CHOICE variant for ") + field_name);
      }
    }

    template <typename T>
    inline void stream_append_assigned_single(PatientStreamSink &dst, FF_FieldKey key, const T &value)
    {
      dst.handle[key] = dst.builder.append_obj(value);
    }

    template <typename T>
    inline void stream_assign_array_offsets(PatientStreamSink &dst, FF_FieldKey key,
                                            const std::vector<T> &values)
    {
      if (values.empty())
        return;
      std::vector<Offset> offsets;
      offsets.reserve(values.size());
      for (const auto &value : values)
      {
        offsets.push_back(dst.builder.append(value));
      }
      dst.handle[key] = offsets;
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_contained(const PatientData &src, Json &dst)
    {
      if (!src.contained.empty())
      {
        dst["contained"] = Json::array();
        for (const auto &c : src.contained)
        {
          dst["contained"].push_back({{"offset", c.offset}, {"recovery", static_cast<int>(c.recovery)}});
        }
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_contained(const PatientData &src, PatientStreamSink &)
    {
      if (!src.contained.empty())
      {
        throw std::runtime_error("FastFHIR benchmark assignment: Patient.contained remap is not implemented for stream assignment");
      }
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_contained(const PatientData &, GooglePatientTarget &)
    {
      // Patient.contained not supported in protobuf arm
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_contained(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "patient.contained", src.contained);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_id(const PatientData &src, Json &dst) { put_if_string(dst, "id", src.id); }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_id(const PatientData &src, PatientStreamSink &dst)
    {
      if (!src.id.empty())
        dst.handle[FastFHIR::Fields::PATIENT::ID] = src.id;
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_id(const PatientData &src, GooglePatientTarget &dst)
    {
      if (!src.id.empty())
        dst.patient.mutable_id()->set_value(std::string(src.id));
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_id(const PatientData &src, HL7v2Sink &dst)
    {
      dst.message.pid.patient_id = std::string(src.id);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_implicit_rules(const PatientData &src, Json &dst)
    {
      put_if_string(dst, "implicitRules", src.implicitrules);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_implicit_rules(const PatientData &src, PatientStreamSink &dst)
    {
      if (!src.implicitrules.empty())
        dst.handle[FastFHIR::Fields::PATIENT::IMPLICIT_RULES] = src.implicitrules;
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_implicit_rules(const PatientData &src, GooglePatientTarget &dst)
    {
      if (!src.implicitrules.empty())
        dst.patient.mutable_implicit_rules()->set_value(std::string(src.implicitrules));
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_implicit_rules(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_string(dst, "patient.implicitRules", src.implicitrules);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_language(const PatientData &src, Json &dst) { put_if_string(dst, "language", src.language); }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_language(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_code_field(dst, FastFHIR::Fields::PATIENT::LANGUAGE, src.language);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_language(const PatientData &src, GooglePatientTarget &dst)
    {
      if (!src.language.empty())
        dst.patient.mutable_language()->set_value(std::string(src.language));
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_language(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_string(dst, "patient.language", src.language);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_active(const PatientData &src, Json &dst) { put_if_bool_flag(dst, "active", src.active); }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_active(const PatientData &src, PatientStreamSink &dst)
    {
      if (src.active != FF_NULL_UINT8)
        dst.handle[FastFHIR::Fields::PATIENT::ACTIVE] = (src.active != 0);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_active(const PatientData &src, GooglePatientTarget &dst)
    {
      if (src.active != FF_NULL_UINT8)
        dst.patient.mutable_active()->set_value(src.active != 0);
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_active(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_bool(dst, "patient.active", src.active);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_gender(const PatientData &src, Json &dst)
    {
      put_if_enum(dst, "gender", static_cast<int>(src.gender));
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_gender(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_code_field(dst, FastFHIR::Fields::PATIENT::GENDER, serialize_AdministrativeGender(src.gender));
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_gender(const PatientData &src, GooglePatientTarget &dst)
    {
      dst.patient.mutable_gender()->set_value(google_map_gender(src.gender));
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_gender(const PatientData &src, HL7v2Sink &dst)
    {
      dst.message.pid.administrative_sex = bench::hl7v2::sex_code(src);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_birth_date(const PatientData &src, Json &dst)
    {
      put_if_string(dst, "birthDate", src.birthdate);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_birth_date(const PatientData &src, PatientStreamSink &dst)
    {
      if (!src.birthdate.empty())
        dst.handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = std::string_view(src.birthdate);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_birth_date(const PatientData &src, GooglePatientTarget &dst)
    {
      // NOT google_birthdate_to_us: that helper returns nullopt when the epoch
      // second is negative, i.e. for ANY DATE BEFORE 1970. In a patient corpus
      // that is most of the population -- two of the five here, born 1951 and
      // 1961, simply had no birthDate on the wire. google_instant_us has no
      // such floor.
      if (src.birthdate.empty()) return;
      google_set_datetime(src.birthdate, dst.patient.mutable_birth_date());
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_birth_date(const PatientData &src, HL7v2Sink &dst)
    {
      dst.message.pid.birth_date = bench::hl7v2::normalize_birthdate(src.birthdate);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_deceased(const PatientData &src, Json &dst) { write_choice(dst, "deceased", src.deceased); }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_deceased(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_choice_field(dst, FastFHIR::Fields::PATIENT::DECEASED, src.deceased, "Patient.deceased");
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_deceased(const PatientData &src, GooglePatientTarget &dst)
    {
      // Was routed through google_birthdate_to_us (pre-1970 floor, date-only)
      // and read the raw string_view alternative, which a packed date/time
      // choice does not use. google_set_choice handles the tag properly.
      if (!src.deceased.is_empty())
        google_set_choice(src.deceased, dst.patient.mutable_deceased());
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_deceased(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_choice(dst, "patient.deceased[x]", src.deceased);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_multiple_birth(const PatientData &src, Json &dst)
    {
      write_choice(dst, "multipleBirth", src.multiplebirth);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_multiple_birth(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_choice_field(dst, FastFHIR::Fields::PATIENT::MULTIPLE_BIRTH, src.multiplebirth, "Patient.multipleBirth");
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_multiple_birth(const PatientData &src, GooglePatientTarget &dst)
    {
      if (!src.multiplebirth.is_empty())
        google_set_choice(src.multiplebirth, dst.patient.mutable_multiple_birth());
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_multiple_birth(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_choice(dst, "patient.multipleBirth[x]", src.multiplebirth);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_meta(const PatientData &src, Json &dst)
    {
      if (src.meta)
        dst["meta"] = to_json_meta(*src.meta);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_meta(const PatientData &src, PatientStreamSink &dst)
    {
      if (src.meta)
        stream_append_assigned_single(dst, FastFHIR::Fields::PATIENT::META, *src.meta);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_meta(const PatientData &src, GooglePatientTarget &dst)
    {
      if (!src.meta) return;
      auto *pb = dst.patient.mutable_meta();
      if (!src.meta->lastupdated.empty())   // see assign_observation_meta
        google_set_instant(src.meta->lastupdated, pb->mutable_last_updated());
      if (!src.meta->versionid.empty())
        pb->mutable_version_id()->set_value(std::string(src.meta->versionid));
      if (!src.meta->source.empty())
        pb->mutable_source()->set_value(std::string(src.meta->source));
      for (const auto &prof : src.meta->profile)
        if (!prof.empty()) pb->add_profile()->set_value(std::string(prof));
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_meta(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "patient.meta", src.meta);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_text(const PatientData &src, Json &dst)
    {
      if (src.text)
        dst["text"] = to_json_narrative(*src.text);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_text(const PatientData &src, PatientStreamSink &dst)
    {
      if (src.text)
        stream_append_assigned_single(dst, FastFHIR::Fields::PATIENT::TEXT, *src.text);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_text(const PatientData &src, GooglePatientTarget &dst)
    {
      if (!src.text) return;
      auto *pb = dst.patient.mutable_text();
      if (!src.text->div.empty()) pb->mutable_div()->set_value(std::string(src.text->div));
      if (src.text->status != FF_NarrativeStatus::FF_UNSET)
        pb->mutable_status()->set_value(
            static_cast<::google::fhir::r4::core::NarrativeStatusCode_Value>(
                src.text->status == FF_NarrativeStatus::Generated
                    ? ::google::fhir::r4::core::NarrativeStatusCode::GENERATED
                : src.text->status == FF_NarrativeStatus::Extensions
                    ? ::google::fhir::r4::core::NarrativeStatusCode::EXTENSIONS
                : src.text->status == FF_NarrativeStatus::Additional
                    ? ::google::fhir::r4::core::NarrativeStatusCode::ADDITIONAL
                    : ::google::fhir::r4::core::NarrativeStatusCode::EMPTY));
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_text(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "patient.text", src.text);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_extension(const PatientData &src, Json &dst)
    {
      if (!src.extension.empty())
      {
        dst["extension"] = Json::array();
        for (const auto &e : src.extension)
          dst["extension"].push_back(to_json_extension(e));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_extension(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::EXTENSION, src.extension);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_extension(const PatientData &src, GooglePatientTarget &dst)
    {
      for (const auto &ext : src.extension)
      {
        google_build_extension(ext, dst.patient.add_extension());
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_extension(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "patient.extension", src.extension);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_modifier_extension(const PatientData &src, Json &dst)
    {
      if (!src.modifierextension.empty())
      {
        dst["modifierExtension"] = Json::array();
        for (const auto &e : src.modifierextension)
          dst["modifierExtension"].push_back(to_json_extension(e));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_modifier_extension(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::MODIFIER_EXTENSION, src.modifierextension);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_modifier_extension(const PatientData &src, GooglePatientTarget &dst)
    {
      for (const auto &ext : src.modifierextension)
      {
        google_build_extension(ext, dst.patient.add_modifier_extension());
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_modifier_extension(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "patient.modifierExtension", src.modifierextension);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_identifier(const PatientData &src, Json &dst)
    {
      if (!src.identifier.empty())
      {
        dst["identifier"] = Json::array();
        for (const auto &i : src.identifier)
          dst["identifier"].push_back(to_json_identifier(i));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_identifier(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::IDENTIFIER, src.identifier);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_identifier(const PatientData &src, GooglePatientTarget &dst)
    {
      for (const auto &identifier : src.identifier)
      {
        auto *pb_id = dst.patient.add_identifier();
        if (!identifier.value.empty())
          pb_id->mutable_value()->set_value(std::string(identifier.value));
        if (!identifier.system.empty())
          pb_id->mutable_system()->set_value(std::string(identifier.system));
        // Identifier.type was never written -- 80 leaves, and it is the field
        // that says WHICH identifier this is (MRN, SSN, driver's licence).
        if (identifier.type)
          google_set_codeable_concept(*identifier.type, pb_id->mutable_type());
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_identifier(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "patient.identifier", src.identifier);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_name(const PatientData &src, Json &dst)
    {
      if (!src.name.empty())
      {
        dst["name"] = Json::array();
        for (const auto &n : src.name)
          dst["name"].push_back(to_json_human_name(n));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_name(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::NAME, src.name);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_name(const PatientData &src, GooglePatientTarget &dst)
    {
      // EVERY name, not just src.name.front(). HumanName is repeated in FHIR and
      // in the proto; taking the first dropped maiden names outright.
      for (const auto &name : src.name)
      {
        auto *out_name = dst.patient.add_name();
        if (!name.text.empty())   out_name->mutable_text()->set_value(std::string(name.text));
        if (!name.family.empty()) out_name->mutable_family()->set_value(std::string(name.family));
        for (const auto &given : name.given)
          if (!given.empty()) out_name->add_given()->set_value(std::string(given));
        for (const auto &prefix : name.prefix)
          if (!prefix.empty()) out_name->add_prefix()->set_value(std::string(prefix));
        for (const auto &suffix : name.suffix)
          if (!suffix.empty()) out_name->add_suffix()->set_value(std::string(suffix));
        if (name.use != FF_NameUse::FF_UNSET)
          out_name->mutable_use()->set_value(google_map_name_use(name.use));
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_name(const PatientData &src, HL7v2Sink &dst)
    {
      dst.message.pid.patient_name = bench::hl7v2::hl7_name_xpn(src);
      // ONE PAYLOAD PER ELEMENT, NEVER TWO. `<field>[*]` carries the WHOLE
      // array and `<field>[0].details` carries its first element, so emitting
      // both writes element 0 twice. The decoder strips both markers before
      // walking, so the two land on the same canonical path and the leaf is
      // counted twice -- 10 duplicate units in this corpus, every one of them a
      // name.
      //
      // The POCO round-trip cannot see this: it rebuilds a struct and re-walks
      // it, which normalises duplicates away, so it stayed at 100%. What sees
      // it is the element tick, and what it would corrupt is the resilience
      // score, where a doubled leaf survives damage twice.
      if (src.name.size() > 1)
      {
        hl7_append_json_field(dst, "patient.name[*]", hl7_json_array(src.name));
      }
      else if (!src.name.empty())
      {
        const auto &name = src.name.front();
        if (!name.id.empty() || !name.extension.empty() || has_u8(static_cast<uint8_t>(name.use)) ||
            !name.text.empty() || name.given.size() > 1 || !name.prefix.empty() ||
            !name.suffix.empty() || name.period)
        {
          hl7_append_json_field(dst, "patient.name[0].details", hl7_json_value(name));
        }
      }
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_telecom(const PatientData &src, Json &dst)
    {
      if (!src.telecom.empty())
      {
        dst["telecom"] = Json::array();
        for (const auto &t : src.telecom)
          dst["telecom"].push_back(to_json_contact_point(t));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_telecom(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::TELECOM, src.telecom);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_telecom(const PatientData &src, GooglePatientTarget &dst)
    {
      for (const auto &telecom : src.telecom)
      {
        auto *pb_contact = dst.patient.add_telecom();
        if (!telecom.value.empty())
          pb_contact->mutable_value()->set_value(std::string(telecom.value));
        if (telecom.system != FF_ContactPointSystem::FF_UNSET)
          pb_contact->mutable_system()->set_value(google_map_contact_system(telecom.system));
        if (telecom.use != FF_ContactPointUse::FF_UNSET)
          pb_contact->mutable_use()->set_value(google_map_contact_use(telecom.use));
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_telecom(const PatientData &src, HL7v2Sink &dst)
    {
      dst.message.pid.home_phone = bench::hl7v2::hl7_phone_xtn(src);
      // Mutually exclusive, for the reason spelled out in assign_patient_name --
      // and here the two payloads were LITERALLY IDENTICAL, both
      // hl7_json_array(src.telecom), so any patient with more than one telecom
      // and one rich entry doubled every telecom leaf. Latent in this corpus
      // (no patient has two), which is exactly why it needed finding by shape
      // rather than by symptom.
      if (src.telecom.size() > 1)
      {
        hl7_append_json_field(dst, "patient.telecom[*]", hl7_json_array(src.telecom));
      }
      else
      {
        for (const auto &telecom : src.telecom)
        {
          if (!telecom.id.empty() || !telecom.extension.empty() ||
              has_u8(static_cast<uint8_t>(telecom.system)) || has_u8(static_cast<uint8_t>(telecom.use)) ||
              has_u32(telecom.rank) || telecom.period)
          {
            hl7_append_json_field(dst, "patient.telecom.details", hl7_json_array(src.telecom));
            break;
          }
        }
      }
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_address(const PatientData &src, Json &dst)
    {
      if (!src.address.empty())
      {
        dst["address"] = Json::array();
        for (const auto &a : src.address)
          dst["address"].push_back(to_json_address(a));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_address(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::ADDRESS, src.address);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_address(const PatientData &src, GooglePatientTarget &dst)
    {
      for (const auto &address : src.address)
      {
        auto *pb_addr = dst.patient.add_address();
        if (!address.text.empty())       pb_addr->mutable_text()->set_value(std::string(address.text));
        if (!address.city.empty())       pb_addr->mutable_city()->set_value(std::string(address.city));
        if (!address.state.empty())      pb_addr->mutable_state()->set_value(std::string(address.state));
        if (!address.postalcode.empty()) pb_addr->mutable_postal_code()->set_value(std::string(address.postalcode));
        if (!address.country.empty())    pb_addr->mutable_country()->set_value(std::string(address.country));
        // Address.line is repeated and was never written -- the street itself.
        for (const auto &line : address.line)
          if (!line.empty()) pb_addr->add_line()->set_value(std::string(line));
        // Synthea hangs the geolocation extension off the address.
        for (const auto &ext : address.extension)
          google_build_extension(ext, pb_addr->add_extension());
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_address(const PatientData &src, HL7v2Sink &dst)
    {
      dst.message.pid.patient_address = bench::hl7v2::hl7_address_xad(src);
      // Mutually exclusive, for the reason spelled out in assign_patient_name.
      if (src.address.size() > 1)
      {
        hl7_append_json_field(dst, "patient.address[*]", hl7_json_array(src.address));
      }
      else if (!src.address.empty())
      {
        const auto &address = src.address.front();
        if (!address.id.empty() || !address.extension.empty() || has_u8(static_cast<uint8_t>(address.use)) ||
            has_u8(static_cast<uint8_t>(address.type)) || !address.text.empty() || address.line.size() > 1 ||
            !address.district.empty() || address.period)
        {
          hl7_append_json_field(dst, "patient.address[0].details", hl7_json_value(address));
        }
      }
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_marital_status(const PatientData &src, Json &dst)
    {
      if (src.maritalstatus)
        dst["maritalStatus"] = to_json_codeable_concept(*src.maritalstatus);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_marital_status(const PatientData &src, PatientStreamSink &dst)
    {
      if (src.maritalstatus)
      {
        stream_append_assigned_single(dst, FastFHIR::Fields::PATIENT::MARITAL_STATUS, *src.maritalstatus);
      }
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_marital_status(const PatientData &src, GooglePatientTarget &dst)
    {
      if (src.maritalstatus)
        google_set_codeable_concept(*src.maritalstatus, dst.patient.mutable_marital_status());
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_marital_status(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "patient.maritalStatus", src.maritalstatus);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_photo(const PatientData &src, Json &dst)
    {
      if (!src.photo.empty())
      {
        dst["photo"] = Json::array();
        for (const auto &p : src.photo)
          dst["photo"].push_back(to_json_attachment(p));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_photo(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::PHOTO, src.photo);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_photo(const PatientData &src, GooglePatientTarget &dst)
    {
      for (const auto &photo : src.photo)
      {
        auto *pb_photo = dst.patient.add_photo();
        if (!photo.contenttype.empty())
          pb_photo->mutable_content_type()->set_value(std::string(photo.contenttype));
        if (!photo.language.empty())
          pb_photo->mutable_language()->set_value(std::string(photo.language));
        // PORT-6: Attachment.data / .hash are std::unique_ptr<std::string_view>.
        if (photo.data && !photo.data->empty())
          pb_photo->mutable_data()->set_value(std::string(*photo.data));
        if (!photo.url.empty())
          pb_photo->mutable_url()->set_value(std::string(photo.url));
        if (photo.size != FF_NULL_UINT32)
          pb_photo->mutable_size()->set_value(photo.size);
        if (photo.hash && !photo.hash->empty())
          pb_photo->mutable_hash()->set_value(std::string(*photo.hash));
        if (!photo.title.empty())
          pb_photo->mutable_title()->set_value(std::string(photo.title));
        // Note: photo.creation omitted here; requires passing through google_birthdate_to_us() and assigning to DateTime
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_photo(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "patient.photo", src.photo);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_contact(const PatientData &src, Json &dst)
    {
      if (!src.contact.empty())
      {
        dst["contact"] = Json::array();
        for (const auto &c : src.contact)
          dst["contact"].push_back(to_json_patient_contact(c));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_contact(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::CONTACT, src.contact);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_contact(const PatientData &src, GooglePatientTarget &dst)
    {
      for (const auto &contact : src.contact)
      {
        auto *pb_contact = dst.patient.add_contact();

        if (contact.gender != FF_AdministrativeGender::Unknown)
        {
          pb_contact->mutable_gender()->set_value(google_map_gender(contact.gender));
        }

        if (contact.name)
        {
          auto *pb_name = pb_contact->mutable_name();
          if (!contact.name->text.empty())
            pb_name->mutable_text()->set_value(std::string(contact.name->text));
          if (!contact.name->family.empty())
            pb_name->mutable_family()->set_value(std::string(contact.name->family));
        }

        if (contact.organization && !contact.organization->reference.empty())
        {
          pb_contact->mutable_organization()->mutable_organization_id()->set_value(std::string(contact.organization->reference));
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_contact(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "patient.contact", src.contact);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_communication(const PatientData &src, Json &dst)
    {
      if (!src.communication.empty())
      {
        dst["communication"] = Json::array();
        for (const auto &c : src.communication)
          dst["communication"].push_back(to_json_patient_communication(c));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_communication(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::COMMUNICATION, src.communication);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_communication(const PatientData &src, GooglePatientTarget &dst)
    {
      for (const auto &comm : src.communication)
      {
        auto *pb_comm = dst.patient.add_communication();
        if (comm.preferred != FF_NULL_UINT8)
          pb_comm->mutable_preferred()->set_value(comm.preferred != 0);
        // Every coding, and `display` with them -- this copied coding.front()
        // and omitted the display outright.
        if (comm.language)
          google_set_codeable_concept(*comm.language, pb_comm->mutable_language());
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_communication(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "patient.communication", src.communication);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_general_practitioner(const PatientData &src, Json &dst)
    {
      if (!src.generalpractitioner.empty())
      {
        dst["generalPractitioner"] = Json::array();
        for (const auto &gp : src.generalpractitioner)
          dst["generalPractitioner"].push_back(to_json_reference(gp));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_general_practitioner(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::GENERAL_PRACTITIONER,
                                  src.generalpractitioner);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_general_practitioner(const PatientData &src, GooglePatientTarget &dst)
    {
      for (const auto &gp : src.generalpractitioner)
      {
        if (!gp.reference.empty())
        {
          auto *pb_gp = dst.patient.add_general_practitioner();
          pb_gp->mutable_practitioner_id()->set_value(std::string(gp.reference));
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_general_practitioner(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "patient.generalPractitioner", src.generalpractitioner);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_managing_organization(const PatientData &src, Json &dst)
    {
      if (src.managingorganization)
        dst["managingOrganization"] = to_json_reference(*src.managingorganization);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_managing_organization(const PatientData &src, PatientStreamSink &dst)
    {
      if (src.managingorganization)
      {
        stream_append_assigned_single(dst, FastFHIR::Fields::PATIENT::MANAGING_ORGANIZATION,
                                      *src.managingorganization);
      }
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_managing_organization(const PatientData &src, GooglePatientTarget &dst)
    {
      if (src.managingorganization && !src.managingorganization->reference.empty())
      {
        dst.patient.mutable_managing_organization()->mutable_organization_id()->set_value(
            std::string(src.managingorganization->reference));
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_managing_organization(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "patient.managingOrganization", src.managingorganization);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_patient_link(const PatientData &src, Json &dst)
    {
      if (!src.link.empty())
      {
        dst["link"] = Json::array();
        for (const auto &l : src.link)
          dst["link"].push_back(to_json_patient_link(l));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_patient_link(const PatientData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::PATIENT::LINK, src.link);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_patient_link(const PatientData &src, GooglePatientTarget &dst)
    {
      for (const auto &link : src.link)
      {
        auto *pb_link = dst.patient.add_link();

        if (link.other && !link.other->reference.empty())
        {
          pb_link->mutable_other()->mutable_patient_id()->set_value(std::string(link.other->reference));
        }

        // 0 is a real ordinal (FastFHIR enums are alphabetical); FF_NULL_UINT8
        // is the absence marker. The cast is left as-is: LinkType is not
        // exercised by this corpus, so an explicit map would be untested code.
        if (has_u8(static_cast<uint8_t>(link.type)))
        {
          pb_link->mutable_type()->set_value(
              static_cast<::google::fhir::r4::core::LinkTypeCode_Value>(static_cast<int>(link.type)));
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_patient_link(const PatientData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "patient.link", src.link);
    }
#endif

    // ------- Observation field assignment (JSON + FFHR stream) -------

#if defined(ARM_JSON)
    inline void assign_observation_contained(const ObservationData &src, Json &dst)
    {
      if (!src.contained.empty())
      {
        dst["contained"] = Json::array();
        for (const auto &c : src.contained)
        {
          dst["contained"].push_back({{"offset", c.offset}, {"recovery", static_cast<int>(c.recovery)}});
        }
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_contained(const ObservationData &src, PatientStreamSink &)
    {
      if (!src.contained.empty())
      {
        throw std::runtime_error("FastFHIR benchmark assignment: Observation.contained remap is not implemented for stream assignment");
      }
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_contained(const ObservationData &, GoogleObservationTarget &)
    {
      // Observation.contained not supported in protobuf arm
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_contained(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.contained", src.contained);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_id(const ObservationData &src, Json &dst) { put_if_string(dst, "id", src.id); }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_id(const ObservationData &src, PatientStreamSink &dst)
    {
      if (!src.id.empty())
        dst.handle[FastFHIR::Fields::OBSERVATION::ID] = src.id;
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_id(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (!src.id.empty())
        dst.observation.mutable_id()->set_value(std::string(src.id));
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_id(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_string(dst, "observation.id", src.id);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_meta(const ObservationData &src, Json &dst)
    {
      if (src.meta)
        dst["meta"] = to_json_meta(*src.meta);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_meta(const ObservationData &src, PatientStreamSink &dst)
    {
      if (src.meta)
        stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::META, *src.meta);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_meta(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (!src.meta) return;
      auto *pb = dst.observation.mutable_meta();
      // GUARD BEFORE mutable_. Protobuf's mutable_<field>() CREATES the
      // submessage and marks it present, and it is evaluated as the ARGUMENT --
      // before the callee can decide there is nothing to write. An unguarded
      // call therefore stamps a default-constructed message onto the wire:
      // last_updated with value_us 0, which prints as "1970-01-01T00:00:00+00:00"
      // and put 1,348 epoch timestamps into a document that has none.
      if (!src.meta->lastupdated.empty())
        google_set_instant(src.meta->lastupdated, pb->mutable_last_updated());
      if (!src.meta->versionid.empty())
        pb->mutable_version_id()->set_value(std::string(src.meta->versionid));
      if (!src.meta->source.empty())
        pb->mutable_source()->set_value(std::string(src.meta->source));
      // Meta.profile is repeated Canonical and was never written: 2,241 leaves.
      for (const auto &prof : src.meta->profile)
        if (!prof.empty()) pb->add_profile()->set_value(std::string(prof));
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_meta(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "observation.meta", src.meta);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_implicit_rules(const ObservationData &src, Json &dst)
    {
      put_if_string(dst, "implicitRules", src.implicitrules);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_implicit_rules(const ObservationData &src, PatientStreamSink &dst)
    {
      if (!src.implicitrules.empty())
        dst.handle[FastFHIR::Fields::OBSERVATION::IMPLICIT_RULES] = src.implicitrules;
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_implicit_rules(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (!src.implicitrules.empty())
        dst.observation.mutable_implicit_rules()->set_value(std::string(src.implicitrules));
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_implicit_rules(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_string(dst, "observation.implicitRules", src.implicitrules);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_language(const ObservationData &src, Json &dst) { put_if_string(dst, "language", src.language); }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_language(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_code_field(dst, FastFHIR::Fields::OBSERVATION::LANGUAGE, src.language);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_language(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (!src.language.empty())
        dst.observation.mutable_language()->set_value(std::string(src.language));
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_language(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_string(dst, "observation.language", src.language);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_text(const ObservationData &src, Json &dst)
    {
      if (src.text)
        dst["text"] = to_json_narrative(*src.text);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_text(const ObservationData &src, PatientStreamSink &dst)
    {
      if (src.text)
        stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::TEXT, *src.text);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_text(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (src.text && !src.text->div.empty())
      {
        dst.observation.mutable_text()->mutable_div()->set_value(std::string(src.text->div));
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_text(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "observation.text", src.text);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_extension(const ObservationData &src, Json &dst)
    {
      if (!src.extension.empty())
      {
        dst["extension"] = Json::array();
        for (const auto &e : src.extension)
          dst["extension"].push_back(to_json_extension(e));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_extension(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::EXTENSION, src.extension);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_extension(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &ext : src.extension)
      {
        google_build_extension(ext, dst.observation.add_extension());
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_extension(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.extension", src.extension);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_modifier_extension(const ObservationData &src, Json &dst)
    {
      if (!src.modifierextension.empty())
      {
        dst["modifierExtension"] = Json::array();
        for (const auto &e : src.modifierextension)
          dst["modifierExtension"].push_back(to_json_extension(e));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_modifier_extension(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::MODIFIER_EXTENSION, src.modifierextension);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_modifier_extension(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &ext : src.modifierextension)
      {
        google_build_extension(ext, dst.observation.add_modifier_extension());
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_modifier_extension(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.modifierExtension", src.modifierextension);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_identifier(const ObservationData &src, Json &dst)
    {
      if (!src.identifier.empty())
      {
        dst["identifier"] = Json::array();
        for (const auto &v : src.identifier)
          dst["identifier"].push_back(to_json_identifier(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_identifier(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::IDENTIFIER, src.identifier);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_identifier(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &identifier : src.identifier)
      {
        if (!identifier.value.empty())
        {
          auto *pb_id = dst.observation.add_identifier();
          pb_id->mutable_value()->set_value(std::string(identifier.value));
          if (!identifier.system.empty())
          {
            pb_id->mutable_system()->set_value(std::string(identifier.system));
          }
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_identifier(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.identifier", src.identifier);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_based_on(const ObservationData &src, Json &dst)
    {
      if (!src.basedon.empty())
      {
        dst["basedOn"] = Json::array();
        for (const auto &v : src.basedon)
          dst["basedOn"].push_back(to_json_reference(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_based_on(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::BASED_ON, src.basedon);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_based_on(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &ref : src.basedon)
      {
        if (!ref.reference.empty())
        {
          auto *pb_ref = dst.observation.add_based_on();
          pb_ref->mutable_procedure_id()->set_value(std::string(ref.reference));
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_based_on(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.basedOn", src.basedon);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_part_of(const ObservationData &src, Json &dst)
    {
      if (!src.partof.empty())
      {
        dst["partOf"] = Json::array();
        for (const auto &v : src.partof)
          dst["partOf"].push_back(to_json_reference(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_part_of(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::PART_OF, src.partof);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_part_of(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &ref : src.partof)
      {
        if (!ref.reference.empty())
        {
          auto *pb_ref = dst.observation.add_part_of();
          pb_ref->mutable_observation_id()->set_value(std::string(ref.reference));
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_part_of(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.partOf", src.partof);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_status(const ObservationData &src, Json &dst)
    {
      put_if_string(dst, "status", serialize_ObservationStatus(src.status));
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_status(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_code_field(dst, FastFHIR::Fields::OBSERVATION::STATUS, serialize_ObservationStatus(src.status));
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_status(const ObservationData &src, GoogleObservationTarget &dst)
    {
      dst.observation.mutable_status()->set_value(google_map_observation_status(src.status));
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_status(const ObservationData &src, HL7v2Sink &dst)
    {
      // Observation.status ordinal 0 is a REAL status, not an absent one.
      if (has_u8(static_cast<uint8_t>(src.status)))
      {
        hl7_append_json_field(dst, "observation.status", hl7_json_value(static_cast<int>(src.status)));
      }
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_category(const ObservationData &src, Json &dst)
    {
      if (!src.category.empty())
      {
        dst["category"] = Json::array();
        for (const auto &v : src.category)
          dst["category"].push_back(to_json_codeable_concept(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_category(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::CATEGORY, src.category);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_category(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &cat : src.category)
        google_set_codeable_concept(cat, dst.observation.add_category());
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_category(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.category", src.category);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_code(const ObservationData &src, Json &dst)
    {
      if (src.code)
        dst["code"] = to_json_codeable_concept(*src.code);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_code(const ObservationData &src, PatientStreamSink &dst)
    {
      if (src.code)
        stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::CODE, *src.code);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_code(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (src.code) google_set_codeable_concept(*src.code, dst.observation.mutable_code());
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_code(const ObservationData &src, HL7v2Sink &dst)
    {
      dst.current_obx.observation_id = bench::hl7v2::observation_code_id(src);
      if (src.code)
      {
        if (!src.code->id.empty() || !src.code->extension.empty() || !src.code->text.empty() ||
            src.code->coding.size() > 1)
        {
          hl7_append_json_field(dst, "observation.code.details", hl7_json_value(*src.code));
        }
        if (!src.code->coding.empty())
        {
          const auto &coding = src.code->coding.front();
          if (!coding.id.empty() || !coding.extension.empty() || !coding.version.empty() ||
              has_u8(coding.userselected))
          {
            hl7_append_json_field(dst, "observation.code.coding[0].details", hl7_json_value(coding));
          }
        }
      }
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_subject(const ObservationData &src, Json &dst)
    {
      if (src.subject)
        dst["subject"] = to_json_reference(*src.subject);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_subject(const ObservationData &src, PatientStreamSink &dst)
    {
      if (src.subject)
        stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::SUBJECT, *src.subject);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_subject(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (src.subject) google_set_reference(*src.subject, dst.observation.mutable_subject());
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_subject(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "observation.subject", src.subject);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_focus(const ObservationData &src, Json &dst)
    {
      if (!src.focus.empty())
      {
        dst["focus"] = Json::array();
        for (const auto &v : src.focus)
          dst["focus"].push_back(to_json_reference(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_focus(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::FOCUS, src.focus);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_focus(const ObservationData &src, GoogleObservationTarget &dst)
    {
      // Observation.focus references: protobuf Reference type requires specialized handling
      for (const auto &focus : src.focus)
      {
        if (!focus.reference.empty())
        {
          auto *pb_focus = dst.observation.add_focus();
          // Populate reference ID field if available in protobuf
          pb_focus->mutable_patient_id()->set_value(std::string(focus.reference));
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_focus(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.focus", src.focus);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_encounter(const ObservationData &src, Json &dst)
    {
      if (src.encounter)
        dst["encounter"] = to_json_reference(*src.encounter);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_encounter(const ObservationData &src, PatientStreamSink &dst)
    {
      if (src.encounter)
        stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::ENCOUNTER, *src.encounter);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_encounter(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (src.encounter) google_set_reference(*src.encounter, dst.observation.mutable_encounter());
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_encounter(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "observation.encounter", src.encounter);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_effective(const ObservationData &src, Json &dst)
    {
      // effective[x] is dateTime | Period | Timing | instant in FHIR. Packed
      // date/time arrives in the hydrated ChoiceEntry as the RAW 63-bit slot
      // value (uint64_t) -- write_choice would emit it as a JSON number, which
      // the census's is_string() check then misses (the Test 3 gate failure,
      // FF 692 vs JSON 0). Decode through the FF_DATETIME machinery so the
      // emitted JSON is the canonical ISO-8601, matching print_json's output
      // on the FF side. Fallback-slot text (held as string_view) passes
      // through byte-exact.
      if (src.effective.is_empty())
        return;
      const auto tag = src.effective.tag;
      if (tag == RECOVER_FF_DATETIME || tag == RECOVER_FF_DATE || tag == RECOVER_FF_TIME ||
          tag == RECOVER_FF_INSTANT)
      {
        if (std::holds_alternative<uint64_t>(src.effective.value))
        {
          const auto parts = FF_UNPACK_DATETIME(std::get<uint64_t>(src.effective.value));
          dst["effectiveDateTime"] = FF_FORMAT_DATETIME(parts, RECOVER_FF_DATETIME);
          return;
        }
        if (std::holds_alternative<std::string_view>(src.effective.value))
        {
          dst["effectiveDateTime"] = std::get<std::string_view>(src.effective.value);
          return;
        }
      }
      else if (tag == RECOVER_FF_STRING &&
               std::holds_alternative<std::string_view>(src.effective.value))
      {
        // A string-tagged effective can only be a datetime fallback slot; no
        // FHIR effective[x] variant is a bare string.
        dst["effectiveDateTime"] = std::get<std::string_view>(src.effective.value);
        return;
      }
      write_choice(dst, "effective", src.effective);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_effective(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_choice_field(dst, FastFHIR::Fields::OBSERVATION::EFFECTIVE, src.effective, "Observation.effective");
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_effective(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (!src.effective.is_empty())
        google_set_choice(src.effective, dst.observation.mutable_effective());
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_effective(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_choice(dst, "observation.effective[x]", src.effective);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_issued(const ObservationData &src, Json &dst) { put_if_string(dst, "issued", src.issued); }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_issued(const ObservationData &src, PatientStreamSink &dst)
    {
      if (!src.issued.empty())
        dst.handle[FastFHIR::Fields::OBSERVATION::ISSUED] = std::string_view(src.issued);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_issued(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (!src.issued.empty())   // mutable_ creates; see assign_observation_meta
        google_set_instant(src.issued, dst.observation.mutable_issued());
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_issued(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_string(dst, "observation.issued", src.issued);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_performer(const ObservationData &src, Json &dst)
    {
      if (!src.performer.empty())
      {
        dst["performer"] = Json::array();
        for (const auto &v : src.performer)
          dst["performer"].push_back(to_json_reference(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_performer(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::PERFORMER, src.performer);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_performer(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &perf : src.performer)
        google_set_reference(perf, dst.observation.add_performer());
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_performer(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.performer", src.performer);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_value(const ObservationData &src, Json &dst) { write_choice(dst, "value", src.value); }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_value(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_choice_field(dst, FastFHIR::Fields::OBSERVATION::VALUE, src.value, "Observation.value");
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_value(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (!src.value.is_empty())
        google_set_choice(src.value, dst.observation.mutable_value());
    }
#elif defined(ARM_HL7V2)
    // The OBX-5/OBX-6 text for one rendered value. An OBX field is
    // pipe-delimited and unescaped by ObxSegment::serialize, so anything
    // heading into it is escaped here.
    inline std::string hl7_obx_text(const Json &v)
    {
      if (v.is_string())  return hl7v2::hl7_escape(v.get<std::string>());
      if (v.is_boolean()) return v.get<bool>() ? "Y" : "N";
      if (v.is_null())    return std::string();
      if (v.is_number())  return hl7v2::hl7_escape(v.dump());
      return hl7v2::hl7_escape(v.dump());
    }

    // Join HL7 components with '^', dropping trailing empties -- v2 does not
    // write separators past the last populated component.
    inline std::string hl7_components(std::initializer_list<std::string> parts)
    {
      std::vector<std::string> v(parts);
      while (!v.empty() && v.back().empty())
        v.pop_back();
      std::string out;
      for (std::size_t i = 0; i < v.size(); ++i)
      {
        if (i) out += '^';
        out += v[i];
      }
      return out;
    }

    // CodeableConcept -> CWE, component order from the CDC converter's
    // datatypes/codeableConcept/CWE.yml: CWE-1 identifier (coding.code),
    // CWE-2 text (coding.display), CWE-3 name of coding system (coding.system).
    inline std::string hl7_cwe(const Json &concept_json)
    {
      if (!concept_json.is_object()) return hl7_obx_text(concept_json);
      const auto codings = concept_json.find("coding");
      if (codings != concept_json.end() && codings->is_array() && !codings->empty())
      {
        const Json &c = codings->front();
        return hl7_components({hl7_obx_text(c.value("code", Json())),
                               hl7_obx_text(c.value("display", Json())),
                               hl7_obx_text(c.value("system", Json()))});
      }
      // No coding: CWE-9 is the original text, but a bare text in CWE-1 is
      // what a receiver without the coding can actually read.
      return hl7_obx_text(concept_json.value("text", Json()));
    }

    // Observation.value[x] -> OBX-2/OBX-5/OBX-6.
    //
    // FOLLOWS bench/vendor/cdc_hl7_mapping/OBXValue.yml (CDC PRIME
    // prime-fhir-converter, Apache-2.0) -- the ReportStream converter, and the
    // only published FHIR->v2 mapping for this element. Rule by rule:
    //
    //   obx-value-nm   OBX-5 = %resource.value.value   (Quantity's number)
    //   obx-value-cwe  OBX-5 = CWE.yml over value      (CodeableConcept)
    //   obx-value-st   OBX-5 = %resource.value         (string)
    //   obx-value-dtm  OBX-5 = the v2 date/time
    //   obx-value-nr   OBX-5 = NR.yml                  (Range)
    //   obx-value-sn   OBX-5 = SN.yml                  (Ratio/structured numeric)
    //   obx-value-dr   OBX-5 = DR.yml                  (Period)
    //
    // WITH ONE DEVIATION, and it is forced. Every condition in that file picks
    // the datatype by reading an extension carrying the ORIGINAL v2 type:
    //
    //   condition: '%context.extension(%`rsext-obx-observation`)
    //                .extension.where(url = "OBX.2").value = "NM"'
    //
    // The converter exists for data that came from v2 and is going back to it.
    // This corpus is Synthea -- native FHIR, never v2, no such extension, so
    // every upstream condition would be false and OBX-5 would come out empty.
    // OBX-2 is therefore INFERRED from the FHIR datatype, using the very
    // correspondence those rules encode. See the vendored README.
    //
    // OBX-6 is NOT mapped upstream at all. A Quantity's unit has nowhere else
    // to go, and OBX-6 is a CWE of units, so it is written as code^^system --
    // marked below as this arm's own, not CDC's.
    //
    // WHAT THIS REPLACED: a stub that wrote OBX-5 = "1" and OBX-6 = "{qty}"
    // for every observation in the corpus, whatever it measured, deriving them
    // from the variant tag alone and never reading `src.value`. The round-trip
    // scored those constants as surviving leaves, so the arm was credited for
    // data it had discarded, and the corruption sweep measured how durably a
    // literal survives bit flips. A visible gap is recoverable; fabricated
    // agreement is not, because nothing downstream can tell it from real data.
    inline void assign_observation_value(const ObservationData &src, HL7v2Sink &dst)
    {
      if (src.value.is_empty())
        return;

      if (src.value.block)
      {
        if (auto rendered = choice_block_json(*src.value.block))
        {
          const Json &v = rendered->value;
          const std::string &type = rendered->suffix;  // the FHIR datatype name

          if (type == "Quantity")
          {
            dst.current_obx.value_type = "NM";                        // obx-value-nm
            dst.current_obx.value = hl7_obx_text(v.value("value", Json()));
            // OBX-6 is a CWE of units: code^text^system. CWE-2 carries the
            // display unit, which was previously left empty -- with it, OBX
            // holds the WHOLE Quantity (value, code, unit, system) and the ZFX
            // passthrough below is not needed for this datatype.
            dst.current_obx.units = hl7_components({
                hl7_obx_text(v.value("code", Json())),
                hl7_obx_text(v.value("unit", Json())),
                hl7_obx_text(v.value("system", Json()))});
            // NO ZFX FOR A QUANTITY.
            //
            // Carrying the value in OBX-5/-6 AND in ZFX meant a blasted OBX
            // resurrected from the passthrough and scored perfect, so the only
            // genuinely-v2 structure on this wire could not be measured or
            // damaged. One carrier per datum: OBX is the native one, and it is
            // sufficient here.
            return;
          }
          else if (type == "CodeableConcept")
          {
            dst.current_obx.value_type = "CWE";                       // obx-value-cwe
            dst.current_obx.value = hl7_cwe(v);
            dst.current_obx.units.clear();
          }
          else if (type == "Range")
          {
            dst.current_obx.value_type = "NR";                        // obx-value-nr
            dst.current_obx.value = hl7_components({
                hl7_obx_text(v.contains("low")  ? v.at("low").value("value", Json())  : Json()),
                hl7_obx_text(v.contains("high") ? v.at("high").value("value", Json()) : Json())});
            dst.current_obx.units.clear();
          }
          else if (type == "Ratio")
          {
            dst.current_obx.value_type = "SN";                        // obx-value-sn
            dst.current_obx.value = hl7_components({
                std::string(),
                hl7_obx_text(v.contains("numerator")   ? v.at("numerator").value("value", Json())   : Json()),
                "/",
                hl7_obx_text(v.contains("denominator") ? v.at("denominator").value("value", Json()) : Json())});
            dst.current_obx.units.clear();
          }
          else if (type == "Period")
          {
            dst.current_obx.value_type = "DR";                        // obx-value-dr
            dst.current_obx.value = hl7_components({hl7_obx_text(v.value("start", Json())),
                                                    hl7_obx_text(v.value("end", Json()))});
            dst.current_obx.units.clear();
          }
          else
          {
            // Every other block datatype: no OBX-5 scalar exists for it, so the
            // segment says ST and the passthrough carries the structure.
            dst.current_obx.value_type = "ST";
            dst.current_obx.value = hl7_obx_text(v.value("text", Json()));
            dst.current_obx.units.clear();
          }

          // OBX-5/-6 cannot hold a Quantity's system or a CodeableConcept's
          // second coding, so the whole value also rides the ZFX passthrough,
          // through the SAME helper every other choice field on this arm uses.
          hl7_mark_if_choice(dst, "observation.value[x]", src.value);
          return;
        }
      }

      // An inline scalar variant: one value, and it fits OBX-5 exactly.
      // obx-value-st / -nm / -dtm, by the FHIR primitive's type.
      const Json wrapped = hl7_json_value(src.value);   // {"value<Suffix>": x}
      Json scalar = Json();
      std::string suffix;
      if (wrapped.is_object() && wrapped.size() == 1)
      {
        suffix = wrapped.begin().key();
        scalar = wrapped.begin().value();
      }
      const bool is_datetime = suffix.find("DateTime") != std::string::npos ||
                               suffix.find("Instant") != std::string::npos ||
                               suffix.find("Date") != std::string::npos;
      dst.current_obx.value_type = scalar.is_number() ? "NM"
                                  : is_datetime       ? "DTM"
                                                      : "ST";
      dst.current_obx.value = hl7_obx_text(scalar);
      dst.current_obx.units.clear();
      hl7_mark_if_choice(dst, "observation.value[x]", src.value);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_data_absent_reason(const ObservationData &src, Json &dst)
    {
      if (src.dataabsentreason)
        dst["dataAbsentReason"] = to_json_codeable_concept(*src.dataabsentreason);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_data_absent_reason(const ObservationData &src, PatientStreamSink &dst)
    {
      if (src.dataabsentreason)
      {
        stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::DATA_ABSENT_REASON, *src.dataabsentreason);
      }
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_data_absent_reason(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (src.dataabsentreason && !src.dataabsentreason->text.empty())
      {
        dst.observation.mutable_data_absent_reason()->mutable_text()->set_value(std::string(src.dataabsentreason->text));
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_data_absent_reason(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "observation.dataAbsentReason", src.dataabsentreason);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_interpretation(const ObservationData &src, Json &dst)
    {
      if (!src.interpretation.empty())
      {
        dst["interpretation"] = Json::array();
        for (const auto &v : src.interpretation)
          dst["interpretation"].push_back(to_json_codeable_concept(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_interpretation(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::INTERPRETATION, src.interpretation);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_interpretation(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &interpretation : src.interpretation)
      {
        if (!interpretation.text.empty() || !interpretation.coding.empty())
        {
          auto *pb_interp = dst.observation.add_interpretation();
          if (!interpretation.text.empty())
          {
            pb_interp->mutable_text()->set_value(std::string(interpretation.text));
          }
          if (!interpretation.coding.empty())
          {
            const auto &c = interpretation.coding.front();
            auto *pb_coding = pb_interp->add_coding();
            if (!c.code.empty())
            {
              pb_coding->mutable_code()->set_value(std::string(c.code));
            }
            if (!c.display.empty())
            {
              pb_coding->mutable_display()->set_value(std::string(c.display));
            }
            if (!c.system.empty())
            {
              pb_coding->mutable_system()->set_value(std::string(c.system));
            }
          }
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_interpretation(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.interpretation", src.interpretation);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_note(const ObservationData &src, Json &dst)
    {
      if (!src.note.empty())
      {
        dst["note"] = Json::array();
        for (const auto &v : src.note)
          dst["note"].push_back(to_json_annotation(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_note(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::NOTE, src.note);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_note(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &note : src.note)
      {
        if (!note.text.empty())
        {
          auto *pb_note = dst.observation.add_note();
          pb_note->mutable_text()->set_value(std::string(note.text));
          // Note: time field is not a simple string in protobuf Annotation; requires DateTime handling
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_note(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.note", src.note);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_body_site(const ObservationData &src, Json &dst)
    {
      if (src.bodysite)
        dst["bodySite"] = to_json_codeable_concept(*src.bodysite);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_body_site(const ObservationData &src, PatientStreamSink &dst)
    {
      if (src.bodysite)
        stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::BODY_SITE, *src.bodysite);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_body_site(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (src.bodysite && !src.bodysite->text.empty())
      {
        dst.observation.mutable_body_site()->mutable_text()->set_value(std::string(src.bodysite->text));
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_body_site(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "observation.bodySite", src.bodysite);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_method(const ObservationData &src, Json &dst)
    {
      if (src.method)
        dst["method"] = to_json_codeable_concept(*src.method);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_method(const ObservationData &src, PatientStreamSink &dst)
    {
      if (src.method)
        stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::METHOD, *src.method);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_method(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (src.method && !src.method->text.empty())
      {
        dst.observation.mutable_method()->mutable_text()->set_value(std::string(src.method->text));
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_method(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "observation.method", src.method);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_specimen(const ObservationData &src, Json &dst)
    {
      if (src.specimen)
        dst["specimen"] = to_json_reference(*src.specimen);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_specimen(const ObservationData &src, PatientStreamSink &dst)
    {
      if (src.specimen)
        stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::SPECIMEN, *src.specimen);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_specimen(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (src.specimen && !src.specimen->reference.empty())
      {
        dst.observation.mutable_specimen()->mutable_specimen_id()->set_value(std::string(src.specimen->reference));
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_specimen(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "observation.specimen", src.specimen);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_device(const ObservationData &src, Json &dst)
    {
      if (src.device)
        dst["device"] = to_json_reference(*src.device);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_device(const ObservationData &src, PatientStreamSink &dst)
    {
      if (src.device)
        stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::DEVICE, *src.device);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_device(const ObservationData &src, GoogleObservationTarget &dst)
    {
      if (src.device && !src.device->reference.empty())
      {
        dst.observation.mutable_device()->mutable_device_id()->set_value(std::string(src.device->reference));
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_device(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "observation.device", src.device);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_reference_range(const ObservationData &src, Json &dst)
    {
      if (!src.referencerange.empty())
      {
        dst["referenceRange"] = Json::array();
        for (const auto &v : src.referencerange)
          dst["referenceRange"].push_back(to_json_observation_reference_range(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_reference_range(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::REFERENCE_RANGE, src.referencerange);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_reference_range(const ObservationData &src, GoogleObservationTarget &dst)
    {
      // Observation.referenceRange: Quantity/Decimal type requires specialized protobuf handling
      // Simplified implementation storing unit information only
      for (const auto &rr : src.referencerange)
      {
        auto *pb_rr = dst.observation.add_reference_range();
        if (rr.low && !rr.low->unit.empty())
        {
          pb_rr->mutable_low()->mutable_unit()->set_value(std::string(rr.low->unit));
        }
        if (rr.high && !rr.high->unit.empty())
        {
          pb_rr->mutable_high()->mutable_unit()->set_value(std::string(rr.high->unit));
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_reference_range(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.referenceRange", src.referencerange);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_has_member(const ObservationData &src, Json &dst)
    {
      if (!src.hasmember.empty())
      {
        dst["hasMember"] = Json::array();
        for (const auto &v : src.hasmember)
          dst["hasMember"].push_back(to_json_reference(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_has_member(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::HAS_MEMBER, src.hasmember);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_has_member(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &member : src.hasmember)
      {
        if (!member.reference.empty())
        {
          auto *pb_member = dst.observation.add_has_member();
          pb_member->mutable_observation_id()->set_value(std::string(member.reference));
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_has_member(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.hasMember", src.hasmember);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_derived_from(const ObservationData &src, Json &dst)
    {
      if (!src.derivedfrom.empty())
      {
        dst["derivedFrom"] = Json::array();
        for (const auto &v : src.derivedfrom)
          dst["derivedFrom"].push_back(to_json_reference(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_derived_from(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::DERIVED_FROM, src.derivedfrom);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_derived_from(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &derived : src.derivedfrom)
      {
        if (!derived.reference.empty())
        {
          auto *pb_derived = dst.observation.add_derived_from();
          pb_derived->mutable_observation_id()->set_value(std::string(derived.reference));
        }
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_derived_from(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.derivedFrom", src.derivedfrom);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_component(const ObservationData &src, Json &dst)
    {
      if (!src.component.empty())
      {
        dst["component"] = Json::array();
        for (const auto &v : src.component)
          dst["component"].push_back(to_json_observation_component(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_component(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::COMPONENT, src.component);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_component(const ObservationData &src, GoogleObservationTarget &dst)
    {
      for (const auto &component : src.component)
      {
        auto *pb_comp = dst.observation.add_component();
        if (component.code)
          google_set_codeable_concept(*component.code, pb_comp->mutable_code());
        // The component's VALUE was never written at all -- 3,600+ leaves.
        if (!component.value.is_empty())
          google_set_choice(component.value, pb_comp->mutable_value());
        for (const auto &interp : component.interpretation)
          google_set_codeable_concept(interp, pb_comp->add_interpretation());
      }
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_component(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.component", src.component);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_instantiates(const ObservationData &src, Json &dst)
    {
      write_choice(dst, "instantiates", src.instantiates);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_instantiates(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_choice_field(dst, FastFHIR::Fields::OBSERVATION::INSTANTIATES, src.instantiates, "Observation.instantiates");
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_instantiates(const ObservationData &, GoogleObservationTarget &) {}
#elif defined(ARM_HL7V2)
    inline void assign_observation_instantiates(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_choice(dst, "observation.instantiates[x]", src.instantiates);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_triggered_by(const ObservationData &src, Json &dst)
    {
      if (!src.triggeredby.empty())
      {
        dst["triggeredBy"] = Json::array();
        for (const auto &v : src.triggeredby)
          dst["triggeredBy"].push_back(to_json_observation_triggered_by(v));
      }
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_triggered_by(const ObservationData &src, PatientStreamSink &dst)
    {
      stream_assign_array_offsets(dst, FastFHIR::Fields::OBSERVATION::TRIGGERED_BY, src.triggeredby);
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_triggered_by(const ObservationData &, GoogleObservationTarget &)
    {
      // Observation.triggeredBy: field not present in this protobuf Observation definition
    }
#elif defined(ARM_HL7V2)
    inline void assign_observation_triggered_by(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_vector(dst, "observation.triggeredBy", src.triggeredby);
    }
#endif

#if defined(ARM_JSON)
    inline void assign_observation_body_structure(const ObservationData &src, Json &dst)
    {
      if (src.bodystructure)
        dst["bodyStructure"] = to_json_reference(*src.bodystructure);
    }
#elif defined(ARM_FASTFHIR)
    inline void assign_observation_body_structure(const ObservationData &src, PatientStreamSink &dst)
    {
      if (src.bodystructure)
      {
        stream_append_assigned_single(dst, FastFHIR::Fields::OBSERVATION::BODY_STRUCTURE, *src.bodystructure);
      }
    }
#elif defined(ARM_GOOGLE_FHIR)
    inline void assign_observation_body_structure(const ObservationData &, GoogleObservationTarget &) {}
#elif defined(ARM_HL7V2)
    inline void assign_observation_body_structure(const ObservationData &src, HL7v2Sink &dst)
    {
      hl7_mark_if_pointer(dst, "observation.bodyStructure", src.bodystructure);
    }
#endif

    template <typename Sink>
    inline void assign_observation_common(const ObservationData &src, Sink &dst)
    {
      assign_observation_contained(src, dst);
      assign_observation_id(src, dst);
      assign_observation_meta(src, dst);
      assign_observation_implicit_rules(src, dst);
      assign_observation_language(src, dst);
      assign_observation_text(src, dst);
      assign_observation_extension(src, dst);
      assign_observation_modifier_extension(src, dst);
      assign_observation_identifier(src, dst);
      assign_observation_based_on(src, dst);
      assign_observation_part_of(src, dst);
      assign_observation_status(src, dst);
      assign_observation_category(src, dst);
      assign_observation_code(src, dst);
      assign_observation_subject(src, dst);
      assign_observation_focus(src, dst);
      assign_observation_encounter(src, dst);
      assign_observation_effective(src, dst);
      assign_observation_issued(src, dst);
      assign_observation_performer(src, dst);
      assign_observation_value(src, dst);
      assign_observation_data_absent_reason(src, dst);
      assign_observation_interpretation(src, dst);
      assign_observation_note(src, dst);
      assign_observation_body_site(src, dst);
      assign_observation_method(src, dst);
      assign_observation_specimen(src, dst);
      assign_observation_device(src, dst);
      assign_observation_reference_range(src, dst);
      assign_observation_has_member(src, dst);
      assign_observation_derived_from(src, dst);
      assign_observation_component(src, dst);
      assign_observation_instantiates(src, dst);
      assign_observation_triggered_by(src, dst);
      assign_observation_body_structure(src, dst);
    }

    template <typename Dst>
    inline void assign_patient_contained_t(const PatientData &src, Dst &dst) { assign_patient_contained(src, dst); }
    template <typename Dst>
    inline void assign_patient_id_t(const PatientData &src, Dst &dst) { assign_patient_id(src, dst); }
    template <typename Dst>
    inline void assign_patient_implicit_rules_t(const PatientData &src, Dst &dst) { assign_patient_implicit_rules(src, dst); }
    template <typename Dst>
    inline void assign_patient_language_t(const PatientData &src, Dst &dst) { assign_patient_language(src, dst); }
    template <typename Dst>
    inline void assign_patient_active_t(const PatientData &src, Dst &dst) { assign_patient_active(src, dst); }
    template <typename Dst>
    inline void assign_patient_gender_t(const PatientData &src, Dst &dst) { assign_patient_gender(src, dst); }
    template <typename Dst>
    inline void assign_patient_birth_date_t(const PatientData &src, Dst &dst) { assign_patient_birth_date(src, dst); }
    template <typename Dst>
    inline void assign_patient_deceased_t(const PatientData &src, Dst &dst) { assign_patient_deceased(src, dst); }
    template <typename Dst>
    inline void assign_patient_multiple_birth_t(const PatientData &src, Dst &dst) { assign_patient_multiple_birth(src, dst); }
    template <typename Dst>
    inline void assign_patient_meta_t(const PatientData &src, Dst &dst) { assign_patient_meta(src, dst); }
    template <typename Dst>
    inline void assign_patient_text_t(const PatientData &src, Dst &dst) { assign_patient_text(src, dst); }
    template <typename Dst>
    inline void assign_patient_extension_t(const PatientData &src, Dst &dst) { assign_patient_extension(src, dst); }
    template <typename Dst>
    inline void assign_patient_modifier_extension_t(const PatientData &src, Dst &dst) { assign_patient_modifier_extension(src, dst); }
    template <typename Dst>
    inline void assign_patient_identifier_t(const PatientData &src, Dst &dst) { assign_patient_identifier(src, dst); }
    template <typename Dst>
    inline void assign_patient_name_t(const PatientData &src, Dst &dst) { assign_patient_name(src, dst); }
    template <typename Dst>
    inline void assign_patient_telecom_t(const PatientData &src, Dst &dst) { assign_patient_telecom(src, dst); }
    template <typename Dst>
    inline void assign_patient_address_t(const PatientData &src, Dst &dst) { assign_patient_address(src, dst); }
    template <typename Dst>
    inline void assign_patient_marital_status_t(const PatientData &src, Dst &dst) { assign_patient_marital_status(src, dst); }
    template <typename Dst>
    inline void assign_patient_photo_t(const PatientData &src, Dst &dst) { assign_patient_photo(src, dst); }
    template <typename Dst>
    inline void assign_patient_contact_t(const PatientData &src, Dst &dst) { assign_patient_contact(src, dst); }
    template <typename Dst>
    inline void assign_patient_communication_t(const PatientData &src, Dst &dst) { assign_patient_communication(src, dst); }
    template <typename Dst>
    inline void assign_patient_general_practitioner_t(const PatientData &src, Dst &dst) { assign_patient_general_practitioner(src, dst); }
    template <typename Dst>
    inline void assign_patient_managing_organization_t(const PatientData &src, Dst &dst) { assign_patient_managing_organization(src, dst); }
    template <typename Dst>
    inline void assign_patient_link_t(const PatientData &src, Dst &dst) { assign_patient_link(src, dst); }

    template <typename Sink>
    inline void assign_patient_common(const PatientData &src, Sink &dst)
    {
      assign_patient_contained_t(src, dst);
      assign_patient_id_t(src, dst);
      assign_patient_implicit_rules_t(src, dst);
      assign_patient_language_t(src, dst);
      assign_patient_active_t(src, dst);
      assign_patient_gender_t(src, dst);
      assign_patient_birth_date_t(src, dst);
      assign_patient_deceased_t(src, dst);
      assign_patient_multiple_birth_t(src, dst);
      assign_patient_meta_t(src, dst);
      assign_patient_text_t(src, dst);
      assign_patient_extension_t(src, dst);
      assign_patient_modifier_extension_t(src, dst);
      assign_patient_identifier_t(src, dst);
      assign_patient_name_t(src, dst);
      assign_patient_telecom_t(src, dst);
      assign_patient_address_t(src, dst);
      assign_patient_marital_status_t(src, dst);
      assign_patient_photo_t(src, dst);
      assign_patient_contact_t(src, dst);
      assign_patient_communication_t(src, dst);
      assign_patient_general_practitioner_t(src, dst);
      assign_patient_managing_organization_t(src, dst);
      assign_patient_link_t(src, dst);
    }

  } // namespace detail

  // ------- Resource assignment entrypoints -------

  template <typename Target>
  inline void assign_patient(const PatientData &src, Target &dst)
  {
#if defined(ARM_JSON)
    dst = detail::Json::object();
    dst["resourceType"] = "Patient";
    detail::assign_patient_common(src, dst);
#elif defined(ARM_FASTFHIR)
    auto *builder = dst.get_builder();
    if (builder == nullptr)
    {
      throw std::runtime_error("assign_patient: ObjectHandle has null builder");
    }
    detail::PatientStreamSink sink{*builder, dst};
    detail::assign_patient_common(src, sink);
#elif defined(ARM_GOOGLE_FHIR)
    detail::assign_patient_common(src, dst);
#elif defined(ARM_HL7V2)
    detail::HL7v2Sink sink{dst};
    detail::assign_patient_common(src, sink);
#else
    static_assert(sizeof(Target) == 0, "Unsupported benchmark assignment arm");
#endif
  }

  template <typename Target>
  inline void assign_observation(const ObservationData &src, Target &dst)
  {
#if defined(ARM_JSON)
    dst = detail::Json::object();
    dst["resourceType"] = "Observation";
    detail::assign_observation_common(src, dst);
#elif defined(ARM_FASTFHIR)
    auto *builder = dst.get_builder();
    if (builder == nullptr)
    {
      throw std::runtime_error("assign_observation: ObjectHandle has null builder");
    }
    detail::PatientStreamSink sink{*builder, dst};
    detail::assign_observation_common(src, sink);
#elif defined(ARM_GOOGLE_FHIR)
    detail::assign_observation_common(src, dst);
#elif defined(ARM_HL7V2)
    detail::HL7v2Sink sink{dst};
    sink.begin_observation();
    detail::assign_observation_common(src, sink);
    sink.finish_observation();
#else
    static_assert(sizeof(Target) == 0, "Unsupported benchmark assignment arm");
#endif
  }

#if defined(ARM_FASTFHIR)
  inline FastFHIR::Reflective::ObjectHandle append_patient_stream(FastFHIR::Builder &builder,
                                                                  const PatientData &src)
  {
    auto handle = builder.append_obj(PatientData{});
    assign_patient(src, handle);
    return handle;
  }

  inline FastFHIR::Reflective::ObjectHandle append_observation_stream(FastFHIR::Builder &builder,
                                                                      const ObservationData &src)
  {
    auto handle = builder.append_obj(ObservationData{});
    assign_observation(src, handle);
    return handle;
  }
#endif

}  // inline namespace BENCH_ARM_NS
} // namespace bench::assign
