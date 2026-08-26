# `hl7v2_message.hpp` — HL7v2 Message Types

> ✅ **Ported 2026-08-25.** `sex_code()` switches on `FF_AdministrativeGender`.
> The enum gained an `FF_UNSET = 255` case that currently falls through to each
> arm's `default:` — deciding what unset means per format is tracked in
> [TASKS.md § PARITY](../TASKS.md).

## Purpose

Defines the **HL7v2 message model** used by the HL7v2 benchmark arm. Provides:

- HL7v2 segment structs (MSH, PID, OBX)
- FHIR→HL7v2 field mapping helpers
- `OruR01Message` builder that assembles a complete ORU^R01 message
- `parse_batch()` function used by Test 3's parse-before-query path

This is a **minimal, purpose-built HL7v2 implementation** — it does not use an external HL7v2 parser library. The segment models are scoped to the fields needed for the benchmark's clinical scenario.

## Segment Types

### `MshSegment` — Message Header

```cpp
struct MshSegment {
  std::string serialize(message_control_id) const;
};
```

Produces: `MSH|^~\\&|BENCH|BENCH|BENCH|BENCH|20260504000000||ORU^R01|<id>|P|2.5`

All fields are hardcoded for benchmarking except the message control ID.

### `PidSegment` — Patient Identification

```cpp
struct PidSegment {
  std::string patient_id;
  std::string patient_name;
  std::string birth_date;
  std::string administrative_sex;
  std::string patient_address;
  std::string home_phone;

  std::string serialize() const;
};
```

Produces: `PID|1||<id>||<name>||<dob>|<sex>|||<address>||<phone>`

### `ObxSegment` — Observation Result

```cpp
struct ObxSegment {
  int set_id = 1;
  std::string value_type = "ST";
  std::string observation_id;
  std::string value;
  std::string units;

  std::string serialize() const;
};
```

Produces: `OBX|<id>|<type>|<obs_id>||<value>|<units>`

### `CustomFieldSegment` — Extension (Z-Segments)

```cpp
struct CustomFieldSegment {
  std::string field_name;
  std::string payload;

  std::string serialize() const;
};
```

Produces: `ZFX|<escaped_name>|<escaped_payload>`

Used for FHIR fields that have no HL7v2 equivalent (e.g., resource IDs not mapped to PID/Observation fields).

## FHIR→HL7v2 Mapping Helpers

### `hl7_escape(src)`

Escapes HL7v2 delimiters in string values:
- `|` → `\F\` (field separator)
- `^` → `\S\` (component separator)
- `&` → `\T\` (subcomponent separator)
- `~` → `\R\` (repetition separator)
- `\r`, `\n` → ` ` (newlines removed)

### `normalize_birthdate(src)`

Strips all non-digit characters from a FHIR date string:
- `"1990-03-21"` → `"19900321"`

### `sex_code(patient)`

Maps `AdministrativeGender` enum to HL7v2 PID-8:
| Gender | Code |
|---|---|
| Male | `M` |
| Female | `F` |
| Other | `O` |
| Unknown/Default | `U` |

### `observation_code_id(observation)`

Builds HL7v2 OBX-3 observation identifier from the Observation's codings:
- `"<code>^<display>^LN"` if LOINC system found with display
- `"<code>^Observation^LN"` if LOINC system found without display
- `"<code>^Observation^99LOCAL"` if non-LOINC coding found
- `"UNK^Observation^99LOCAL"` fallback

### `hl7_name_xpn(patient)`

Builds HL7v2 XPN patient name: `"<family>^<given>"`

### `hl7_address_xad(patient)`

Builds HL7v2 XAD address: `"<line>^^<city>^<state>^<postal>^<country>"`

### `hl7_phone_xtn(patient)`

Returns first telecom value found (HL7v2 XTN format — just the number).

## `OruR01Message` — Complete Message Builder

```cpp
struct OruR01Message {
  MshSegment msh;
  PidSegment pid;
  std::vector<ObxSegment> obx;
  std::vector<CustomFieldSegment> custom_fields;

  void append_custom_field(field_name, payload);
  std::string dump() const;
};
```

`dump()` assembles the complete message with:
1. MSH segment (with auto-generated message control ID from microsecond timestamp)
2. PID segment
3. All OBX segments (one per observation)
4. All ZFX custom field segments
5. Segments separated by `\r`, message terminated by `\r`

## Parsing — Parse Tree Structures and `parse_batch()`

### Parse Tree Types

The parser builds a four-level AST from pipe-delimited HL7v2 messages:

```cpp
struct Component {
  std::string_view val;
  std::vector<std::string_view> subcomponents;   // split on '&'
};

struct Field {
  std::string_view val;
  std::vector<Component> components;              // split on '^'

  std::string_view get_component(std::size_t index) const;
};

struct Segment {
  std::string_view name;
  std::vector<Field> fields;                      // split on '|'

  const Field* get_field(std::size_t index) const;  // 1-based
};

struct MessageTree {
  std::vector<Segment> segments;
};

struct ParsedMessage {
  std::string storage;
  MessageTree tree;
};
```

The split spacing (`|` → `^` → `&`) mirrors the HL7v2 encoding hierarchy. Only
structs with multiple children allocate the child vector — a field with no `^`
characters stores only its `.val` with an empty `.components` vector.

### Typed Segment Views

For ergonomic field access, the header provides typed view adapters over
`Segment` nodes. Each exposes HL7v2 field semantics as named accessors:

| View | Accessor | HL7v2 Field | Returns |
|---|---|---|---|
| **`PidView`** | `.patient_id()` | PID-3 (CX) | component 1 |
| | `.family_name()` | PID-5 (XPN) | component 1 |
| | `.given_name()` | PID-5 (XPN) | component 2 |
| | `.birth_date()` | PID-7 (DTM) | field value |
| | `.sex()` | PID-8 (IS) | field value |
| **`ObxView`** | `.value_type()` | OBX-2 (ID) | field value |
| | `.observation_id()` | OBX-3 (CE) | component 1 |
| | `.value()` | OBX-5 (string) | field value |
| | `.units()` | OBX-6 (CE) | component 1 |
| **`ObrView`** | `.filler_order_number()` | OBR-3 (EI) | component 1 |
| | `.service_id()` | OBR-4 (CE) | component 1 |
| | `.service_name()` | OBR-4 (CE) | component 2 |
| **`Pv1View`** | `.patient_class()` | PV1-2 (ID) | field value |
| | `.point_of_care()` | PV1-3 (PL) | component 1 |
| | `.room()` | PV1-3 (PL) | component 2 |
| **`Pr1View`** | `.procedure_code()` | PR1-3 (CE) | component 1 |
| | `.procedure_description()` | PR1-3 (CE) | component 2 |
| | `.procedure_datetime()` | PR1-5 (DTM) | field value |

### `parse_batch()` — Stream to Message List

```cpp
std::vector<ParsedMessage> parse_batch(std::string_view batch);
```

Splits a concatenated HL7v2 stream (multiple ORU^R01 messages) by:
1. **Message boundaries** — each `MSH|` that follows a `\r` (or is at position 0)
   starts a new message.
2. **Within a message** — `parse_message_into_tree()` splits each segment on
   `\r`, then `parse_segment_line()` splits on `|`, `^`, `&` into the four-level
   `MessageTree` (Segment → Field → Component → subcomponent).

Returns a vector of `ParsedMessage` — each owning its string storage and a
fully-parsed `MessageTree`.

### Internal Parsing Pipeline

```
find_message_starts(payload)         → MSH boundary positions
  parse_message(message_view)        → copies string into ParsedMessage::storage
    parse_message_into_tree(view)    → splits on \r, calls parse_segment_line per segment
      parse_segment_line(line)       → splits on |, calls parse_field per token
        parse_field(token)           → splits on ^, calls parse_component per token
          parse_component(token)     → splits on &, assigns subcomponents
```

Each `parse_*` function is short-circuiting: if the delimiter doesn't appear,
the value is stored and the child vector stays empty.

## Usage in Benchmark

In `bench_test_2.hpp` (HL7v2 arm) — full tree walk:

```cpp
auto parsed_messages = hl7v2::parse_batch(payload);
for (auto& msg : parsed_messages) {
  touch_tree(msg, touched_nodes);
  // touch_tree walks: segment → fields → components → subcomponents
}
```

In `bench_test_3.hpp` (HL7v2 arm) — typed field extraction:

```cpp
auto messages = hl7v2::parse_batch(payload);
for (auto& msg : messages) {
  for (const auto& seg : msg.tree.segments) {
    if (seg.name == "PID") {
      hl7v2::PidView pid(seg);
      acc.note_patient(pid.birth_date());
    }
    if (seg.name == "OBX") {
      hl7v2::ObxView obx(seg);
      // Extract LOINC code from OBX-3 component 1
    }
  }
}
```

## Key Design Decisions

| Decision | Rationale |
|---|---|
| No external HL7v2 library | Keeps build dependencies minimal; the message model is simple enough for this benchmark |
| Only ORU^R01 message type | The benchmark focuses on lab observations — ORU^R01 is the natural HL7v2 message type |
| `\r` segment terminators | HL7v2 standard requires `\r` (0x0D) between segments; the benchmark follows the spec |
| ZFX custom segments for unmapped fields | FHIR fields without HL7v2 equivalents (e.g., encounter references) are preserved in Z-segments |
| Microsecond-based message control IDs | Unique per message within a batch; uses `std::chrono` for zero-collision probability |

## Limitations

- **No HL7v2 escaping in parsing**: The parser is a simple segment splitter. Escaped characters (`\F\`, `\S\`, etc.) are not unescaped during parsing — the benchmark's query extracts raw string values.
- **No support for HL7v2.5+ features**: Only HL7v2.5 fields defined. No Z-segment standard mapping, no OBX-4 (sub-ID), no NTE segments.
