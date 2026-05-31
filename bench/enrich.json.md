# `enrich.json` — Enrichment Test Fixture

## Purpose

A static FHIR R4 Observation JSON resource used as the **enrichment fixture** for Test 4 (Enrich) across all four benchmark arms.

This represents a Basic Metabolic Panel (BMP) observation that gets appended to each arm's serialized bundle to measure the cost of "add a lab result to an existing record."

## Resource Type

```json
{
  "resourceType": "Observation",
  "id": "bmp-panel-example",
  "status": "final",
  "code": {
    "coding": [{
      "system": "http://loinc.org",
      "code": "24321-3",
      "display": "Basic metabolic 1998 panel - Serum or Plasma"
    }],
    "text": "Basic Metabolic Panel"
  },
  ...
}
```

## Clinical Context

- **LOINC code**: `24321-3` — Basic Metabolic 1998 Panel
- **Status**: `final` — completed lab result
- **Category**: `laboratory` (US Core profile)
- **Profile**: `http://hl7.org/fhir/us/core/StructureDefinition/us-core-observation-lab`

## Structure

The fixture contains:

| Field | Value | Notes |
|---|---|---|
| `resourceType` | `"Observation"` | Root type |
| `id` | `"bmp-panel-example"` | Stable identifier for cross-arm comparison |
| `meta.profile` | `[us-core-observation-lab]` | US Core lab profile |
| `status` | `"final"` | Completed observation |
| `category[0]` | `laboratory` | Observation category |
| `code` | LOINC `24321-3` | Basic Metabolic Panel code |
| `subject` | Reference to `Patient/c1977187-...` | Generic test patient |
| `effectiveDateTime` | `"2026-05-06T13:09:00Z"` | When the panel was collected |
| `issued` | `"2026-05-06T13:15:00Z"` | When the result was issued |
| `component[]` | 8 component observations | Individual BMP analytes |

## Component Analytes

The panel contains 8 components, each with a `valueQuantity`:

| # | LOINC | Display | Value | Unit |
|---|---|---|---|---|
| 1 | `2345-7` | Glucose | 95 | mg/dL |
| 2 | `17861-1` | Calcium | 9.2 | mg/dL |
| 3 | `2951-2` | Sodium | 140 | mmol/L |
| 4 | `2823-3` | Potassium | 4.1 | mmol/L |
| 5 | `2075-0` | Chloride | 102 | mmol/L |
| 6 | `2028-9` | Carbon dioxide, total | 24 | mmol/L |
| 7 | `3094-0` | Urea nitrogen | 15 | mg/dL |
| 8 | `2160-0` | Creatinine | 1.0 | mg/dL |

## Usage in Benchmark

Each arm's `test_4` enrichment function:

1. **Loads** `enrich.json` via `load_enrichment_observation_from_json()` (→ `EnrichmentObservationFixture`)
2. **Extracts** the `ObservationData` POCO
3. **Appends** it to the arm's serialized bundle payload
4. **Reports** source_bytes, enriched_bytes, appended count, and duration

## Key Design Decisions

| Decision | Rationale |
|---|---|
| BMP panel (not single analyte) | More realistic — real enrichments often add multi-component panels |
| No LOINC `2085-9` in enrichment fixture | Avoids skewing the query test's LOINC match counts; enrichment is a distinct workflow |
| Usable across all 4 arms | The fixture is consumed as a POCO `ObservationData` struct, which each arm serializes in its own format |
