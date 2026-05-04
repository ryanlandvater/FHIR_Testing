#pragma once

#include "harness.hpp"

#include <cctype>
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
  std::string observation_id;
  std::string value;
  std::string units;

  std::string serialize() const {
    return std::string("OBX|") + std::to_string(set_id) + "|" + value_type + "|" +
           observation_id + "||" + value + "|" + units;
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
    case AdministrativeGender::Male:
      return "M";
    case AdministrativeGender::Female:
      return "F";
    case AdministrativeGender::Other:
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

}  // namespace bench::hl7v2
