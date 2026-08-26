#pragma once

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <memory>
#include <stdexcept>
#include <variant>
#include <string>
#include <string_view>
#include <vector>

#include <FastFHIR.hpp>
#include <FF_Bundle.hpp>
#include <FF_Condition.hpp>
#include <FF_Encounter.hpp>
#include <FF_FieldKeys.hpp>
#include <FF_Observation.hpp>
#include <FF_Patient.hpp>
#include <FF_Procedure.hpp>

namespace bench {

// Timed sections are declared here first for manual reviewability.
// Test 1 start: immediately before first field write to destination representation.
// Test 1 end: immediately after payload sealed.
// Test 2 start: immediately before deserialize/materialize work begins.
// Test 2 end: when the in-memory representation is available for query logic.
// Test 3 start: first parser/read call that consumes bytes or materialized nodes for query.
// Test 3 end: target value extracted into result variable.

enum class Stage {
  Test1Serialize,
  Test2Materialize,
  Test3Query,
  Test4Enrich,

  // Temporary compatibility aliases during the stage -> test migration.
  Stage1Serialize = Test1Serialize,
  Stage2Transport = Test2Materialize,
  Stage3Query = Test3Query,
  Stage3Materialize = Test2Materialize
};

struct MetricEvent {
  std::string arm;
  Stage stage;
  std::int64_t duration_ns;
};

struct ArmRunResult {
  std::vector<MetricEvent> metrics;
  std::string queried_value;
  std::string reconstructed_bundle_json;
  std::variant<std::monostate, FastFHIR::Memory, std::string> enriched_stream;
  std::string enrich_metrics_summary;
};

class Timer {
 public:
  void start() { begin_ = std::chrono::steady_clock::now(); }
  std::int64_t stop_ns() const {
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin_).count();
    if (elapsed_ns <= 0) {
      return 1;
    }
    return elapsed_ns;
  }

 private:
  std::chrono::steady_clock::time_point begin_{};
};

inline std::string to_string(Stage s) {
  switch (s) {
    case Stage::Test1Serialize:
      return "test_1_serialize";
    case Stage::Test2Materialize:
      return "test_2_materialize";
    case Stage::Test3Query:
      return "test_3_query";
    case Stage::Test4Enrich:
      return "test_4_enrich";
  }
  return "unknown";
}

inline void print_metric(const MetricEvent& e) {
  std::cout << e.arm << "," << to_string(e.stage) << "," << e.duration_ns << "\n" << std::flush;
}

inline constexpr std::string_view kPatientQueryField = "birthDate";
inline constexpr std::string_view kCholesterolLoincCode = "2085-9";
inline constexpr std::string_view kLoincSystem = "http://loinc.org";

// ---------------------------------------------------------------------------
// FastFHIR stream helpers (ported to the FF_* facade 2026-08-25)
// ---------------------------------------------------------------------------
// Builder::set_root() and Builder::finalize() became private in FastFHIR
// a9fd4e9. The only way in is the friend free functions FF_StreamSetRoot /
// FF_StreamFinalize, which take an FF_Stream -- and FF_Stream is exactly
// std::shared_ptr<Builder>. So we build the shared_ptr ourselves rather than
// going through FF_CreateStream: that keeps the existing "the fixture owns the
// arena, the builder borrows it" model, which BundlePatient::memory and
// clone_bundle_patient() both depend on.

inline FastFHIR::FF_Stream make_stream(const FastFHIR::Memory& memory,
                                       FHIR_VERSION version = FHIR_VERSION_R5) {
  return std::make_shared<FastFHIR::Builder>(memory, version);
}

// Benchmark-mode checksum. FF_StreamFinalize requires a hasher whenever the
// algorithm is not NONE; hashing for real would measure OpenSSL rather than
// FastFHIR, so this returns a zeroed digest of the right width. The stream
// still carries a SHA256 header, exactly as it did before the port.
inline std::vector<BYTE> bench_null_sha256(const unsigned char*, Size) {
  return std::vector<BYTE>(32, 0);
}

// Assign the root and seal the arena. Throws with the FF_Result message on
// failure -- the facade is noexcept and returns status codes, so an unchecked
// call would silently produce an unsealed stream and a fast, meaningless
// number.
inline FastFHIR::Memory::View seal_stream(const FastFHIR::FF_Stream& stream,
                                          const FastFHIR::Reflective::ObjectHandle& root,
                                          std::string_view context,
                                          FF_Checksum_Algorithm algorithm = FF_CHECKSUM_SHA256) {
  FF_Result result = FastFHIR::FF_StreamSetRoot(FastFHIR::FF_StreamSetRootInfo{
      .stream = stream,
      .root = root,
  });
  if (!result) {
    throw std::runtime_error("FF_StreamSetRoot failed for " + std::string(context) + ": " +
                             result.message);
  }

  FastFHIR::Memory::View view;
  result = FastFHIR::FF_StreamFinalize(
      FastFHIR::FF_StreamFinalizeInfo{
          .stream = stream,
          .algorithm = algorithm,
          // A hasher is required only when the algorithm is not NONE.
          .hasher = algorithm == FF_CHECKSUM_NONE ? FastFHIR::FF_HashCallback{}
                                                  : FastFHIR::FF_HashCallback{bench_null_sha256},
      },
      view);
  if (!result) {
    throw std::runtime_error("FF_StreamFinalize failed for " + std::string(context) + ": " +
                             result.message);
  }
  return view;
}

// Reads a FHIR text-valued field off a FastFHIR node.
//
// Upstream DT-2 packs date / dateTime / time / instant into an inline 8-byte
// slot, so Node::as<std::string_view>() -- which demands a string-layout
// recovery tag -- throws "Node is not a string or code" on Patient.birthDate.
// Only values that do not fit the packed form spill to an FF_STRING, and the
// parser hands those back already tagged as strings, so the fast path below
// still covers them.
//
// There is no public zero-copy accessor that renders a packed date/time as
// text; print_json is the only public path, and it costs an ostringstream plus
// JSON escaping that the string case does not pay. That asymmetry is a real
// measurement distortion in the FastFHIR arm's query stage -- see notes.md,
// "Packed date/time has no zero-copy public reader".
inline std::string read_text_field(const FastFHIR::Reflective::Node& node) {
  if (!node) {
    return {};
  }
  if (node.kind() == FF_FIELD_DATETIME) {
    std::ostringstream oss;
    node.print_json(oss);
    std::string text = oss.str();
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
      return text.substr(1, text.size() - 2);
    }
    return text;
  }
  return std::string(node.as<std::string_view>());
}

// ---------------------------------------------------------------------------
// Cross-arena choice[x] sanitisation -- READ THIS BEFORE TRUSTING TEST 1.
// ---------------------------------------------------------------------------
// FastFHIR's generated deserializer stores a BLOCK-typed choice variant
// (valueQuantity, valueCodeableConcept, valuePeriod, ...) as the raw child
// OFFSET of that block, in the ChoiceEntry's uint64_t alternative:
//
//     generated_src/FF_Observation.cpp:319
//         else data.value.value = child_off;   // offset into the SOURCE arena
//
// The POCO carries no base pointer, so that offset is meaningless outside the
// arena it was read from -- and this benchmark hydrates POCOs from one arena
// and serializes them into another, which is precisely the unsupported move.
//
// The generated STORE then writes the foreign offset straight back out and
// tags the slot as a block, so the damage is not confined to code the benchmark
// controls: appending a hydrated POCO wholesale via Builder::append() is enough
// to corrupt the destination stream. validate_FFHR_stream() reports "the offset
// chain is broken", and reading it back segfaults in
// FF_CODEABLECONCEPT::deserialize.
//
// Clearing these at hydration is the only place that fixes every path at once,
// and it has a second benefit: all four arms then receive byte-identical POCOs,
// which is what the parity axiom actually requires.
//
// COST: Test 1 does not measure value[x] serialization at all. Fixing it needs
// the source arena base plumbed through so the block can be deep-copied. See
// notes.md, "Block-typed choice[x] cannot cross arenas".
// True when a ChoiceEntry's payload is an ARENA-RELATIVE reference rather than
// a self-contained value, and therefore cannot be re-serialized into a
// different arena.
//
// Three shapes reach the uint64_t alternative, and only some are portable:
//
//   BLOCK    always a child offset into the source arena         -> NOT portable
//   CODE     dictionary index (MSB clear)                        -> portable
//            packed FF_CODEABLE_CONCEPT offset (MSB set)         -> NOT portable
//   DATETIME packed civil value (bit 63 clear)                   -> portable
//            fallback offset to an FF_STRING (bit 63 set)        -> NOT portable
//
// Missing the CODE case is what kept this bug alive through the first three
// attempts at a fix: Synthea's us-core-race / us-core-ethnicity extensions carry
// valueCode, so ~337 slots per corpus slipped through a BLOCK-only test and
// corrupted the stream anyway.
inline bool is_cross_arena_choice(const ChoiceEntry& choice) {
  if (choice.is_empty() || !std::holds_alternative<uint64_t>(choice.value)) {
    return false;
  }
  const uint64_t raw = std::get<uint64_t>(choice.value);
  switch (Recovery_to_Kind(choice.tag)) {
    case FF_FIELD_BLOCK:
      return true;
    case FF_FIELD_CODE:
      return (static_cast<uint32_t>(raw) & FF_CODEABLE_CONCEPT_FLAG) != 0;
    case FF_FIELD_DATETIME:
      return raw != FF_DATETIME_NULL && FF_DATETIME_IS_FALLBACK(raw);
    default:
      return false;
  }
}

// Retained name for the arm-side call sites.
inline bool is_cross_arena_block_choice(const ChoiceEntry& choice) {
  return is_cross_arena_choice(choice);
}

inline void sanitize_choice(ChoiceEntry& choice) {
  if (is_cross_arena_block_choice(choice)) {
    choice = ChoiceEntry{};
  }
}

inline void sanitize_extensions(std::vector<ExtensionData>& extensions) {
  for (auto& ext : extensions) {
    sanitize_choice(ext.value);
    sanitize_extensions(ext.extension);
  }
}

// Every FHIR datatype carries its own `extension` vector, so an offending
// ChoiceEntry can hide arbitrarily deep -- Synthea alone puts extensions on
// Patient, Patient.address (geolocation) and Patient.name. Sweep each nested
// vector rather than only the top-level ones.
template <typename T>
inline void sanitize_each(std::vector<T>& items) {
  for (auto& item : items) {
    sanitize_extensions(item.extension);
  }
}

inline void sanitize_codeable_concept(CodeableConceptData& concept_data) {
  sanitize_extensions(concept_data.extension);
  sanitize_each(concept_data.coding);
}

inline void sanitize_reference_range(ObservationreferenceRangeData& range) {
  sanitize_extensions(range.extension);
  sanitize_extensions(range.modifierextension);
  if (range.low) sanitize_extensions(range.low->extension);
  if (range.high) sanitize_extensions(range.high->extension);
  if (range.type) sanitize_codeable_concept(*range.type);
  for (auto& applies : range.appliesto) sanitize_codeable_concept(applies);
  if (range.age) sanitize_extensions(range.age->extension);
}

inline void sanitize_observation(ObservationData& observation) {
  sanitize_choice(observation.value);
  sanitize_choice(observation.effective);
  sanitize_choice(observation.instantiates);
  sanitize_extensions(observation.extension);
  sanitize_extensions(observation.modifierextension);

  if (observation.meta) sanitize_extensions(observation.meta->extension);
  if (observation.text) sanitize_extensions(observation.text->extension);
  sanitize_each(observation.identifier);
  sanitize_each(observation.basedon);
  sanitize_each(observation.partof);
  sanitize_each(observation.focus);
  sanitize_each(observation.performer);
  sanitize_each(observation.note);
  sanitize_each(observation.hasmember);
  sanitize_each(observation.derivedfrom);
  sanitize_each(observation.triggeredby);

  for (auto& category : observation.category) sanitize_codeable_concept(category);
  for (auto& interpretation : observation.interpretation) sanitize_codeable_concept(interpretation);
  if (observation.code) sanitize_codeable_concept(*observation.code);
  if (observation.dataabsentreason) sanitize_codeable_concept(*observation.dataabsentreason);
  if (observation.bodysite) sanitize_codeable_concept(*observation.bodysite);
  if (observation.method) sanitize_codeable_concept(*observation.method);

  if (observation.subject) sanitize_extensions(observation.subject->extension);
  if (observation.encounter) sanitize_extensions(observation.encounter->extension);
  if (observation.specimen) sanitize_extensions(observation.specimen->extension);
  if (observation.device) sanitize_extensions(observation.device->extension);
  if (observation.bodystructure) sanitize_extensions(observation.bodystructure->extension);

  for (auto& range : observation.referencerange) sanitize_reference_range(range);

  for (auto& component : observation.component) {
    sanitize_choice(component.value);
    sanitize_extensions(component.extension);
    sanitize_extensions(component.modifierextension);
    if (component.code) sanitize_codeable_concept(*component.code);
    if (component.dataabsentreason) sanitize_codeable_concept(*component.dataabsentreason);
    for (auto& interpretation : component.interpretation) sanitize_codeable_concept(interpretation);
    for (auto& range : component.referencerange) sanitize_reference_range(range);
  }
}

inline void sanitize_patient(PatientData& patient) {
  sanitize_choice(patient.deceased);
  sanitize_choice(patient.multiplebirth);
  sanitize_extensions(patient.extension);
  sanitize_extensions(patient.modifierextension);

  sanitize_each(patient.identifier);
  sanitize_each(patient.name);
  sanitize_each(patient.telecom);
  sanitize_each(patient.address);
  sanitize_each(patient.photo);
  sanitize_each(patient.contact);
  sanitize_each(patient.communication);
  sanitize_each(patient.generalpractitioner);
  sanitize_each(patient.link);

  if (patient.meta) sanitize_extensions(patient.meta->extension);
  if (patient.text) sanitize_extensions(patient.text->extension);
  if (patient.maritalstatus) sanitize_codeable_concept(*patient.maritalstatus);
  if (patient.managingorganization) sanitize_extensions(patient.managingorganization->extension);

  for (auto& contact : patient.contact) {
    sanitize_each(contact.relationship);
    sanitize_each(contact.telecom);
    if (contact.name) sanitize_extensions(contact.name->extension);
    if (contact.address) sanitize_extensions(contact.address->extension);
  }
  for (auto& communication : patient.communication) {
    if (communication.language) sanitize_codeable_concept(*communication.language);
  }
}

// One ingested Synthea patient item kept in RAM.
// The FastFHIR::Memory arena contains the serialized FFHR patient binary.
struct BundlePatient {
  FastFHIR::Memory memory;
  PatientData patient;
  std::vector<EncounterData> encounters;
  std::vector<ConditionData> conditions;
  std::vector<ProcedureData> procedures;
  std::vector<ObservationData> observations;
};

struct BundleBenchFixture {
  std::vector<BundlePatient> bundle;
  int64_t target_size_bytes = 0;
  int64_t actual_ingested_bytes = 0;
  int64_t fastfhir_vma_bytes = 0;
};

struct EnrichmentObservationFixture {
  FastFHIR::Memory memory;
  ObservationData observation;
};

namespace detail {
void hydrate_bundle_resources(const FastFHIR::Reflective::Node& root,
                              BundlePatient& bundle_patient);
}

BundlePatient make_bundle_patient_from_json(const std::filesystem::path& json_path);
EnrichmentObservationFixture load_enrichment_observation_from_json(const std::filesystem::path& json_path);

inline BundlePatient clone_bundle_patient(const BundlePatient& src) {
  BundlePatient dst{};
  dst.memory = src.memory;

  // Re-hydrate the POCO bundle from copied FFHR memory so cloned fixtures keep
  // string-backed fields valid after arena duplication.
  if (dst.memory) {
    FastFHIR::Parser parser(dst.memory);
    detail::hydrate_bundle_resources(parser.root(), dst);
  }

  // Fallback for defensive compatibility if re-hydration cannot locate a patient.
  if (dst.patient.id.empty() && !src.patient.id.empty()) {
    dst.patient.id = src.patient.id;
  }
  if (dst.patient.birthdate.empty() && !src.patient.birthdate.empty()) {
    dst.patient.birthdate = src.patient.birthdate;
  }
  dst.patient.gender = src.patient.gender;
  dst.patient.active = src.patient.active;
  return dst;
}

ArmRunResult run_fastfhir_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_json_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_hl7v2_bundle(const BundleBenchFixture& fixture);
ArmRunResult run_google_fhir_bundle(const BundleBenchFixture& fixture);

// ---------------------------------------------------------------------------
// Cross-arm result validation
// ---------------------------------------------------------------------------

namespace detail {

inline bool reference_matches_patient_id(const ReferenceData& reference,
                                         std::string_view patient_id) {
  if (patient_id.empty() || reference.reference.empty()) {
    return false;
  }

  if (reference.reference == patient_id) {
    return true;
  }

  constexpr std::string_view kPatientPrefix = "Patient/";
  return reference.reference.size() == kPatientPrefix.size() + patient_id.size() &&
         reference.reference.substr(0, kPatientPrefix.size()) == kPatientPrefix &&
         reference.reference.substr(kPatientPrefix.size()) == patient_id;
}

template <typename Resource>
inline void copy_resources_for_patient(const std::vector<FastFHIR::Reflective::Node>& resource_nodes,
                                       std::string_view patient_id,
                                       std::vector<Resource>& out) {
  out.clear();
  out.reserve(resource_nodes.size());

  for (const auto& resource_node : resource_nodes) {
    auto resource = resource_node.as<Resource>();
    if (!patient_id.empty() && resource.subject) {
      if (!reference_matches_patient_id(*resource.subject, patient_id)) {
        continue;
      }
    }
    out.push_back(std::move(resource));
  }
}

inline void hydrate_bundle_resources(const FastFHIR::Reflective::Node& root,
                                     BundlePatient& bundle_patient) {
  bundle_patient.patient = PatientData{};
  bundle_patient.encounters.clear();
  bundle_patient.conditions.clear();
  bundle_patient.procedures.clear();
  bundle_patient.observations.clear();

  FastFHIR::Reflective::Node patient_node;
  std::vector<FastFHIR::Reflective::Node> encounter_nodes;
  std::vector<FastFHIR::Reflective::Node> condition_nodes;
  std::vector<FastFHIR::Reflective::Node> procedure_nodes;
  std::vector<FastFHIR::Reflective::Node> observation_nodes;

  if (root && root.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
    patient_node = root;
  } else if (root && root.is<FastFHIR::RESOURCETYPE::BUNDLE>()) {
    if (auto entries = root[FastFHIR::Fields::BUNDLE::ENTRY]) {
      for (auto& entry : entries.entries()) {
        auto resource = entry[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
        if (!resource) {
          continue;
        }

        auto resource_node = resource.as_node();
        if (!resource_node) {
          continue;
        }

        if (!patient_node && resource_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
          patient_node = resource_node;
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::ENCOUNTER>()) {
          encounter_nodes.push_back(resource_node);
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::CONDITION>()) {
          condition_nodes.push_back(resource_node);
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::PROCEDURE>()) {
          procedure_nodes.push_back(resource_node);
          continue;
        }
        if (resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) {
          observation_nodes.push_back(resource_node);
        }
      }
    }
  }

  if (patient_node && patient_node.is<FastFHIR::RESOURCETYPE::PATIENT>()) {
    bundle_patient.patient = patient_node.as<PatientData>();
  }

  copy_resources_for_patient(encounter_nodes, bundle_patient.patient.id, bundle_patient.encounters);
  copy_resources_for_patient(condition_nodes, bundle_patient.patient.id, bundle_patient.conditions);
  copy_resources_for_patient(procedure_nodes, bundle_patient.patient.id, bundle_patient.procedures);
  copy_resources_for_patient(observation_nodes, bundle_patient.patient.id, bundle_patient.observations);

  // Drop choice[x] variants that carry a source-arena offset. They cannot be
  // re-serialized into another arena, and leaving them in corrupts the FastFHIR
  // arm's stream and feeds the other arms a raw offset as if it were a value.
  // Doing it here means every arm receives byte-identical POCOs. See the
  // sanitize_choice() commentary above and notes.md.
  sanitize_patient(bundle_patient.patient);
  for (auto& observation : bundle_patient.observations) {
    sanitize_observation(observation);
    if (std::getenv("BENCH_DROP_OBS_CODE")) observation.code.reset();
    if (std::getenv("BENCH_DROP_OBS_CAT")) observation.category.clear();
    if (std::getenv("BENCH_DROP_OBS_COMP")) observation.component.clear();
    if (std::getenv("BENCH_DROP_OBS_EXT")) { observation.extension.clear(); observation.modifierextension.clear(); }
    if (std::getenv("BENCH_DROP_OBS_RR")) observation.referencerange.clear();
    if (std::getenv("BENCH_DROP_OBS_INTERP")) observation.interpretation.clear();
  }
}

// Extract a key=value field from a queried_value string.
inline std::string qv_field(const std::string& qv, const std::string& key) {
  const std::string needle = key + "=";
  const auto pos = qv.find(needle);
  if (pos == std::string::npos) return "";
  const auto start = pos + needle.size();
  const auto end = qv.find(' ', start);
  return end == std::string::npos ? qv.substr(start) : qv.substr(start, end - start);
}

// Reduce a date string to its digit characters for format-agnostic comparison.
inline std::string digits_only(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) if (std::isdigit(c)) out += static_cast<char>(c);
  return out;
}

} // namespace detail

// Compare the query results across FFHR and JSON arms and print a warning to stderr
// for each discrepancy.  Returns true only if every check passes.
//
// Checks performed:
//   1. patients=N   — both arms must agree
//   2. birthdate    — normalized to digits; both arms must agree
//   3. encounters   — when present in both arms, values must agree
//   4. conditions   — when present in both arms, values must agree
//   5. observations — required in both arms and values must agree
//   6. loinc_2085_9_matches — required in both arms and values must agree
//   7. Observation value/effective/component semantic counters — required parity
inline bool validate_results(const ArmRunResult& ff, const ArmRunResult& jf) {
  using detail::qv_field;
  using detail::digits_only;
  bool ok = true;

  auto compare_required_field = [&](const char* key) {
    const auto ff_value = qv_field(ff.queried_value, key);
    const auto jf_value = qv_field(jf.queried_value, key);
    if (ff_value.empty() || jf_value.empty()) {
      std::cerr << "[validate] MISSING required field " << key << ":"
                << " fastfhir=" << (ff_value.empty() ? "<missing>" : ff_value)
                << " json=" << (jf_value.empty() ? "<missing>" : jf_value) << "\n";
      ok = false;
      return;
    }
    if (ff_value != jf_value) {
      std::cerr << "[validate] MISMATCH " << key << ":"
                << " fastfhir=" << ff_value
                << " json=" << jf_value << "\n";
      ok = false;
    }
  };

  // 1. Patient count
  const auto ff_pat = qv_field(ff.queried_value, "patients");
  const auto jf_pat = qv_field(jf.queried_value, "patients");
  if (ff_pat != jf_pat) {
    std::cerr << "[validate] MISMATCH patients:"
              << " fastfhir=" << ff_pat
              << " json="     << jf_pat << "\n";
    ok = false;
  }

  // 2. Birthdate (spot-check first patient; format-normalised)
  const auto ff_bd = digits_only(qv_field(ff.queried_value, "birthdate"));
  const auto jf_bd = digits_only(qv_field(jf.queried_value, "birthdate"));
  if (!ff_bd.empty() && ff_bd != jf_bd) {
    std::cerr << "[validate] MISMATCH birthdate:"
              << " fastfhir=" << ff_bd
              << " json="     << jf_bd << "\n";
    ok = false;
  }

  // 3. Encounter count (optional until all arms emit it)
  const auto ff_enc = qv_field(ff.queried_value, "encounters");
  const auto jf_enc = qv_field(jf.queried_value, "encounters");
  if (!ff_enc.empty() && !jf_enc.empty()) {
    if (ff_enc != jf_enc) {
      std::cerr << "[validate] MISMATCH encounters:"
                << " fastfhir=" << ff_enc
                << " json="     << jf_enc << "\n";
      ok = false;
    }
  }

  // 4. Condition count (optional until all arms emit it)
  const auto ff_cond = qv_field(ff.queried_value, "conditions");
  const auto jf_cond = qv_field(jf.queried_value, "conditions");
  if (!ff_cond.empty() && !jf_cond.empty()) {
    if (ff_cond != jf_cond) {
      std::cerr << "[validate] MISMATCH conditions:"
                << " fastfhir=" << ff_cond
                << " json="     << jf_cond << "\n";
      ok = false;
    }
  }

  compare_required_field("observations");
  compare_required_field("loinc_2085_9_matches");
  compare_required_field("obs_value_present");
  compare_required_field("obs_value_quantity");
  compare_required_field("obs_value_codeableconcept");
  compare_required_field("obs_value_string");
  compare_required_field("obs_value_code");
  compare_required_field("obs_effective_datetime");
  compare_required_field("obs_effective_period");
  compare_required_field("obs_issued_present");
  compare_required_field("obs_component_value_present");
  compare_required_field("obs_component_value_quantity");
  compare_required_field("obs_component_value_codeableconcept");
  compare_required_field("obs_component_value_string");
  compare_required_field("obs_component_value_code");

  // 6. Reconstructed fixture parity (both arms must reconstruct identical bundle JSON)
  if (!ff.reconstructed_bundle_json.empty() && !jf.reconstructed_bundle_json.empty()) {
    if (ff.reconstructed_bundle_json != jf.reconstructed_bundle_json) {
      std::cerr << "[validate] MISMATCH reconstructed_bundle_json:"
                << " fastfhir=" << ff.reconstructed_bundle_json.size()
                << " json=" << jf.reconstructed_bundle_json.size() << "\n";
      ok = false;
    }
  }

  return ok;
}

// Compare HL7v2 query outputs against FFHR/JSON baseline for the subset of
// fields that HL7v2 currently models directly.
inline bool validate_hl7_results(const ArmRunResult& ff,
                                 const ArmRunResult& jf,
                                 const ArmRunResult& h2) {
  using detail::digits_only;
  using detail::qv_field;

  bool ok = true;
  auto compare_required_field = [&](const char* key) {
    const auto baseline_value = qv_field(jf.queried_value, key);
    const auto hl7_value = qv_field(h2.queried_value, key);
    if (baseline_value.empty() || hl7_value.empty()) {
      std::cerr << "[validate] MISSING hl7 required field " << key << ":"
                << " baseline=" << (baseline_value.empty() ? "<missing>" : baseline_value)
                << " hl7=" << (hl7_value.empty() ? "<missing>" : hl7_value) << "\n";
      ok = false;
      return;
    }
    if (baseline_value != hl7_value) {
      std::cerr << "[validate] MISMATCH hl7 " << key << ":"
                << " baseline=" << baseline_value
                << " hl7=" << hl7_value << "\n";
      ok = false;
    }
  };

  compare_required_field("patients");

  const auto baseline_birthdate = digits_only(qv_field(jf.queried_value, "birthdate"));
  const auto hl7_birthdate = digits_only(qv_field(h2.queried_value, "birthdate"));
  if (baseline_birthdate.empty() || hl7_birthdate.empty()) {
    std::cerr << "[validate] MISSING hl7 required field birthdate:"
              << " baseline=" << (baseline_birthdate.empty() ? "<missing>" : baseline_birthdate)
              << " hl7=" << (hl7_birthdate.empty() ? "<missing>" : hl7_birthdate) << "\n";
    ok = false;
  } else if (baseline_birthdate != hl7_birthdate) {
    std::cerr << "[validate] MISMATCH hl7 birthdate:"
              << " baseline=" << baseline_birthdate
              << " hl7=" << hl7_birthdate << "\n";
    ok = false;
  }

  compare_required_field("observations");
  compare_required_field("loinc_2085_9_matches");
  compare_required_field("obs_value_present");
  compare_required_field("obs_value_quantity");
  compare_required_field("obs_value_codeableconcept");
  compare_required_field("obs_value_string");
  compare_required_field("obs_value_code");
  compare_required_field("obs_effective_datetime");
  compare_required_field("obs_effective_period");
  compare_required_field("obs_issued_present");
  compare_required_field("obs_component_value_present");
  compare_required_field("obs_component_value_quantity");
  compare_required_field("obs_component_value_codeableconcept");
  compare_required_field("obs_component_value_string");
  compare_required_field("obs_component_value_code");

  // Keep FFHR input referenced to make baseline dependency explicit and avoid
  // accidental drift where only one arm is checked.
  (void)ff;
  return ok;
}

}  // namespace bench
