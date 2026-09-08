#pragma once

#include "harness.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace bench::hl7v2 {

static inline std::string hl7_escape(std::string_view src) {
  std::string out;
  out.reserve(src.size());
  for (const char c : src) {
    switch (c) {
      case '|':
        out += "\\F\\";
        break;
      case '^':
        out += "\\S\\";
        break;
      case '&':
        out += "\\T\\";
        break;
      case '~':
        out += "\\R\\";
        break;
      case '\r':
      case '\n':
        out += ' ';
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

// The inverse of hl7_escape. Note it is not total: hl7_escape maps both \r and
// \n to a space, and that is not recoverable -- which is correct for HL7v2,
// where a bare carriage return IS the segment terminator and cannot appear in
// a field.
static inline std::string unescape_text(std::string_view src) {
  std::string out;
  out.reserve(src.size());
  for (std::size_t i = 0; i < src.size(); ++i) {
    if (src[i] == '\\' && i + 2 < src.size() && src[i + 2] == '\\') {
      switch (src[i + 1]) {
        case 'F': out.push_back('|'); i += 2; continue;
        case 'S': out.push_back('^'); i += 2; continue;
        case 'T': out.push_back('&'); i += 2; continue;
        case 'R': out.push_back('~'); i += 2; continue;
        case 'E': out.push_back('\\'); i += 2; continue;
        default: break;
      }
    }
    out.push_back(src[i]);
  }
  return out;
}

struct MshSegment {
  std::string serialize(std::string_view message_control_id) const {
    // MSH fields: 1='|' and 2='^~\&' are encoded after segment name.
    return std::string("MSH|^~\\&|BENCH|BENCH|BENCH|BENCH|20260504000000||ORU^R01|") +
           std::string(message_control_id) + "|P|2.5";
  }
};

struct PidSegment {
  std::string patient_id;
  std::string patient_name;
  std::string birth_date;
  std::string administrative_sex;
  std::string patient_address;
  std::string home_phone;

  std::string serialize() const {
    // PID.3 patient ID, PID.5 name, PID.7 DOB, PID.8 sex, PID.11 address, PID.13 phone.
    return std::string("PID|1||") + patient_id +
           "||" + patient_name +
           "||" + birth_date +
           "|" + administrative_sex +
           "|||" + patient_address +
           "||" + home_phone;
  }
};

struct ObxSegment {
  int set_id = 1;
  std::string value_type = "ST";
  std::string observation_id;   // OBX-3: the LOINC code identifying the test
  std::string sub_id;           // OBX-4: which Observation resource this row is
  std::string value;
  std::string units;

  // OBX-4 (Observation Sub-ID) carries the FHIR Observation.id.
  //
  // It was empty, so a reader had to recover the row's owner from its ORDINAL
  // position -- OBX-1 indexing into the observations in message order. That
  // makes attribution global: one damaged observation id shifts every later
  // OBX onto the wrong resource, and the values do not go missing, they move.
  // Measured at 64 flips, that produced 571 changed leaves -- units migrating
  // between results ("g/dL" -> "mmol/L", "%" -> "g/dL"), which is the worst
  // failure mode a clinical format has. Naming the owner in the row makes the
  // damage local: a corrupted OBX-4 loses that row and nothing else.
  std::string serialize() const {
    return std::string("OBX|") + std::to_string(set_id) + "|" + value_type + "|" +
           observation_id + "|" + sub_id + "|" + value + "|" + units;
  }
};

struct CustomFieldSegment {
  std::string field_name;
  std::string payload;

  std::string serialize() const {
    return std::string("ZFX|") + hl7_escape(field_name) + "|" + hl7_escape(payload);
  }
};

static inline std::string normalize_birthdate(std::string_view src) {
  std::string out;
  out.reserve(src.size());
  for (const char c : src) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

static inline std::string sex_code(const PatientData& patient) {
  switch (patient.gender) {
    case FF_AdministrativeGender::Male:
      return "M";
    case FF_AdministrativeGender::Female:
      return "F";
    case FF_AdministrativeGender::Other:
      return "O";
    default:
      return "U";
  }
}

static inline std::string observation_code_id(const ObservationData& observation) {
  if (!observation.code) {
    return "UNK^Observation^99LOCAL";
  }

  for (const auto& coding : observation.code->coding) {
    if (coding.code.empty()) {
      continue;
    }
    if (coding.system.empty() || coding.system == kLoincSystem) {
      if (!coding.display.empty()) {
        return std::string(coding.code) + "^" + std::string(coding.display) + "^LN";
      }
      return std::string(coding.code) + "^Observation^LN";
    }
  }

  for (const auto& coding : observation.code->coding) {
    if (!coding.code.empty()) {
      return std::string(coding.code) + "^Observation^99LOCAL";
    }
  }

  return "UNK^Observation^99LOCAL";
}

static inline std::string hl7_name_xpn(const PatientData& patient) {
  if (patient.name.empty()) {
    return "";
  }

  const auto& name = patient.name.front();
  std::string given;
  if (!name.given.empty()) {
    given = std::string(name.given.front());
  }
  return std::string(name.family) + "^" + given;
}

static inline std::string hl7_address_xad(const PatientData& patient) {
  if (patient.address.empty()) {
    return "";
  }

  const auto& address = patient.address.front();
  std::string line;
  if (!address.line.empty()) {
    line = std::string(address.line.front());
  }
  return line + "^^" + std::string(address.city) + "^" + std::string(address.state) + "^" +
         std::string(address.postalcode) + "^" + std::string(address.country);
}

static inline std::string hl7_phone_xtn(const PatientData& patient) {
  if (patient.telecom.empty()) {
    return "";
  }

  for (const auto& telecom : patient.telecom) {
    if (!telecom.value.empty()) {
      return std::string(telecom.value);
    }
  }
  return "";
}

struct OruR01Message {
  MshSegment msh;
  PidSegment pid;
  std::vector<ObxSegment> obx;
  std::vector<CustomFieldSegment> custom_fields;

  void append_custom_field(std::string_view field_name, std::string payload) {
    custom_fields.push_back(CustomFieldSegment{std::string(field_name), std::move(payload)});
  }

  std::string dump() const {
    std::string out;
    out.reserve(256 + obx.size() * 64 + custom_fields.size() * 96);
    out += msh.serialize(pid.patient_id);
    out += '\r';
    out += pid.serialize();
    out += '\r';
    for (const auto& seg : obx) {
      out += seg.serialize();
      out += '\r';
    }
    for (const auto& seg : custom_fields) {
      out += seg.serialize();
      out += '\r';
    }
    return out;
  }
};

// ==========================================
// Zero-copy parse-side structures
// ==========================================

struct Component {
  std::string_view val;
  std::vector<std::string_view> subcomponents;

  void dump(std::string& out) const {
    if (subcomponents.empty()) {
      out.append(val);
      return;
    }
    for (std::size_t i = 0; i < subcomponents.size(); ++i) {
      out.append(subcomponents[i]);
      if (i + 1 < subcomponents.size()) {
        out.push_back('&');
      }
    }
  }
};

struct Field {
  std::string_view val;
  std::vector<Component> components;

  std::string_view get_component(std::size_t index) const {
    if (index == 0 || components.empty() || index > components.size()) {
      return val;
    }
    return components[index - 1].val;
  }

  void dump(std::string& out) const {
    if (components.empty()) {
      out.append(val);
      return;
    }
    for (std::size_t i = 0; i < components.size(); ++i) {
      components[i].dump(out);
      if (i + 1 < components.size()) {
        out.push_back('^');
      }
    }
  }
};

struct Segment {
  std::string_view name;
  std::vector<Field> fields;

  const Field* get_field(std::size_t index) const {
    if (index == 0 || index > fields.size()) {
      return nullptr;
    }
    return &fields[index - 1];
  }

  void dump(std::string& out) const {
    out.append(name);
    for (const auto& field : fields) {
      out.push_back('|');
      field.dump(out);
    }
  }
};

struct MessageTree {
  std::vector<Segment> segments;

  std::string dump() const {
    std::string out;
    out.reserve(2048);
    for (const auto& seg : segments) {
      seg.dump(out);
      out.push_back('\r');
    }
    return out;
  }
};

class PidView {
 public:
  explicit PidView(const Segment& segment) : seg_(segment) {}

  std::string_view patient_id() const {
    if (const auto* f = seg_.get_field(3)) {
      return f->get_component(1);
    }
    return {};
  }

  std::string_view family_name() const {
    if (const auto* f = seg_.get_field(5)) {
      return f->get_component(1);
    }
    return {};
  }

  std::string_view given_name() const {
    if (const auto* f = seg_.get_field(5)) {
      return f->get_component(2);
    }
    return {};
  }

  std::string_view birth_date() const {
    if (const auto* f = seg_.get_field(7)) {
      return f->val;
    }
    return {};
  }

  std::string_view sex() const {
    if (const auto* f = seg_.get_field(8)) {
      return f->val;
    }
    return {};
  }

 private:
  const Segment& seg_;
};

class Pv1View {
 public:
  explicit Pv1View(const Segment& segment) : seg_(segment) {}

  std::string_view patient_class() const {
    if (const auto* f = seg_.get_field(2)) {
      return f->val;
    }
    return {};
  }

  std::string_view point_of_care() const {
    if (const auto* f = seg_.get_field(3)) {
      return f->get_component(1);
    }
    return {};
  }

  std::string_view room() const {
    if (const auto* f = seg_.get_field(3)) {
      return f->get_component(2);
    }
    return {};
  }

 private:
  const Segment& seg_;
};

class ObrView {
 public:
  explicit ObrView(const Segment& segment) : seg_(segment) {}

  std::string_view filler_order_number() const {
    if (const auto* f = seg_.get_field(3)) {
      return f->get_component(1);
    }
    return {};
  }

  std::string_view service_id() const {
    if (const auto* f = seg_.get_field(4)) {
      return f->get_component(1);
    }
    return {};
  }

  std::string_view service_name() const {
    if (const auto* f = seg_.get_field(4)) {
      return f->get_component(2);
    }
    return {};
  }

 private:
  const Segment& seg_;
};

class ObxView {
 public:
  explicit ObxView(const Segment& segment) : seg_(segment) {}

  std::string_view value_type() const {
    if (const auto* f = seg_.get_field(2)) {
      return f->val;
    }
    return {};
  }

  std::string_view observation_id() const {
    if (const auto* f = seg_.get_field(3)) {
      return f->get_component(1);
    }
    return {};
  }

  std::string_view value() const {
    if (const auto* f = seg_.get_field(5)) {
      return f->val;
    }
    return {};
  }

  std::string_view units() const {
    if (const auto* f = seg_.get_field(6)) {
      return f->get_component(1);
    }
    return {};
  }

 private:
  const Segment& seg_;
};

class Pr1View {
 public:
  explicit Pr1View(const Segment& segment) : seg_(segment) {}

  std::string_view procedure_code() const {
    if (const auto* f = seg_.get_field(3)) {
      return f->get_component(1);
    }
    return {};
  }

  std::string_view procedure_description() const {
    if (const auto* f = seg_.get_field(3)) {
      return f->get_component(2);
    }
    return {};
  }

  std::string_view procedure_datetime() const {
    if (const auto* f = seg_.get_field(5)) {
      return f->val;
    }
    return {};
  }

 private:
  const Segment& seg_;
};

struct ParsedMessage {
  std::string storage;
  MessageTree tree;
};

inline bool is_message_start(std::string_view payload, std::size_t pos) {
  if (pos + 4 > payload.size()) {
    return false;
  }
  if (payload[pos] != 'M' || payload[pos + 1] != 'S' || payload[pos + 2] != 'H' ||
      payload[pos + 3] != '|') {
    return false;
  }
  return pos == 0 || payload[pos - 1] == '\r';
}

inline std::vector<std::size_t> find_message_starts(std::string_view payload) {
  std::vector<std::size_t> starts;
  if (payload.size() < 4) {
    return starts;
  }
  for (std::size_t i = 0; i + 3 < payload.size(); ++i) {
    if (is_message_start(payload, i)) {
      starts.push_back(i);
    }
  }
  return starts;
}

inline std::vector<std::string_view> split_escaped(std::string_view src, char delimiter,
                                                   char escape_char = '\\') {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  bool in_escape = false;

  for (std::size_t i = 0; i < src.size(); ++i) {
    if (src[i] == escape_char) {
      in_escape = !in_escape;
      continue;
    }
    if (!in_escape && src[i] == delimiter) {
      out.push_back(src.substr(start, i - start));
      start = i + 1;
    }
  }
  out.push_back(src.substr(start));
  return out;
}

inline Component parse_component(std::string_view token) {
  Component component;
  component.val = token;
  const auto subcomponents = split_escaped(token, '&');
  if (subcomponents.size() > 1) {
    component.subcomponents = std::move(subcomponents);
  }
  return component;
}

inline Field parse_field(std::string_view token) {
  Field field;
  field.val = token;
  const auto components = split_escaped(token, '^');
  if (components.size() > 1) {
    field.components.reserve(components.size());
    for (const auto component : components) {
      field.components.push_back(parse_component(component));
    }
  }
  return field;
}

inline Segment parse_segment_line(std::string_view line) {
  Segment segment;
  if (line.empty()) {
    return segment;
  }

  const auto tokens = split_escaped(line, '|');
  if (tokens.empty()) {
    return segment;
  }

  segment.name = tokens[0];
  if (tokens.size() > 1) {
    segment.fields.reserve(tokens.size() - 1);
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      segment.fields.push_back(parse_field(tokens[i]));
    }
  }
  return segment;
}

inline void parse_message_into_tree(std::string_view message, MessageTree& tree) {
  std::size_t line_start = 0;
  while (line_start < message.size()) {
    const std::size_t line_end = message.find('\r', line_start);
    const std::size_t end = line_end == std::string_view::npos ? message.size() : line_end;
    const std::string_view line = message.substr(line_start, end - line_start);
    if (!line.empty()) {
      tree.segments.push_back(parse_segment_line(line));
    }

    if (line_end == std::string_view::npos) {
      break;
    }
    line_start = line_end + 1;
  }
}

inline ParsedMessage parse_message(std::string_view message) {
  ParsedMessage parsed;
  parsed.storage.assign(message.data(), message.size());
  parse_message_into_tree(std::string_view(parsed.storage), parsed.tree);
  return parsed;
}

inline std::vector<ParsedMessage> parse_batch(std::string_view payload) {
  std::vector<ParsedMessage> messages;
  const auto starts = find_message_starts(payload);
  if (starts.empty()) {
    return messages;
  }

  messages.reserve(starts.size());
  for (std::size_t i = 0; i < starts.size(); ++i) {
    const std::size_t begin = starts[i];
    const std::size_t end = (i + 1 < starts.size()) ? starts[i + 1] : payload.size();
    if (end > begin) {
      messages.push_back(parse_message(payload.substr(begin, end - begin)));
    }
  }
  return messages;
}

}  // namespace bench::hl7v2
