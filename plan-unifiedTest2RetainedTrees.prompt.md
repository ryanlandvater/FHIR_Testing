## Plan: Unified Test 2 Retained Trees

Redesign Test 2 around one shared API pattern and one shared result contract, while keeping each arm’s native payload type. The implementation should deserialize/materialize the arm payload into a retained in-memory tree, eagerly touch the full tree during Test 2, retain ownership until the test ends, and return real Test 2 metrics. This preserves your stage boundaries without forcing FastFHIR to change its Stage 1 output shape.

**Steps**
1. Phase 1: define the shared Test 2 contract in bench_test_2.hpp.
2. Replace the placeholder with one common entry function name, exposed as arm-specific overloads or macro-selected overloads under the same `template<T>materialize(...)` API shape where `T` is the arm-specific stream type (FastFHIR::Memory or std::string).
3. Define one shared result wrapper pattern, such as `MaterializedTree`, with identical top-level fields across arms: retained ownership object, touched-node count, parse/materialization status, and any arm-local tree handle needed by Stage 3.
4. Define one shared metric helper, such as `materialize_metric(...)`, so all arms record Test 2 identically.
5. Preserve timing boundaries exactly: timing starts immediately before Test 2 parse/materialize begins and stops only after the full retained tree has been built and eagerly touched.

6. Phase 2: implement eager retained-tree materialization per arm behind the shared contract.
7. FastFHIR branch: accept `FastFHIR::Memory`, construct parser/root, recursively walk the reflective tree via `entries()` only, and retain the parser plus any retained node roots needed so the full tree remains valid through end of test.
8. JSON branch: accept the JSON payload string, parse once with simdjson DOM, recursively walk every object/array/value to force full tree access, and retain parser plus DOM/document ownership through end of test.
9. HL7v2 branch: accept the HL7 payload string, deserialize into a retained in-memory representation that is queryable without rescanning the raw string in Stage 3. Prefer a full segment/field tree that retains all parsed field values, not a deferred line-scan view.
10. Reuse as much structure as possible between arms by standardizing the wrapper layout and recursive-touch accounting, even though the internal node types differ under macros.

11. Phase 3: rewire arm runners and Stage 3 consumption.
12. Update arm runners to call the shared Test 2 entry pattern and store the returned materialized tree until the end of the run.
13. Pass the retained tree, not the raw payload, into Stage 3 query overloads so Stage 3 excludes deserialize/materialization work.
14. Keep Stage 1 payload generation unchanged, per your decision to keep arm-native payload types.

15. Phase 4: query adapters and parity hardening.
16. Add Stage 3 query overloads in bench_test_3.hpp for the materialized tree wrappers while preserving current output semantics.
17. Ensure each arm’s Stage 2 eagerly touches the complete reachable tree and retains it long enough for Stage 3 and any post-query validation.
18. Keep HL7, JSON, and FastFHIR parity focused on full-tree retention and eager traversal, not on forcing identical internal container types.

19. Phase 5: verification.
20. Run timing conformance and bench harness smoke tests to confirm all arms emit real Test 2 metrics and that Stage 3 no longer reparses raw payloads.
21. Validate parity outputs across arms for birthdate, observation counts, and other existing query summary fields.
22. Inspect timed paths to confirm no FFHR-to-JSON conversion bridge appears and no arm defers parse work into Stage 3.

**Relevant files**
- /Users/RyanLandvater/Programming_Projects/FHIR_Testing/bench/bench_test_2.hpp
  Define the shared Test 2 API, shared wrapper shape, and arm-specific retained-tree implementations.
- /Users/RyanLandvater/Programming_Projects/FHIR_Testing/bench/arm_fastfhir.cpp
  Hold the returned Test 2 retained tree alive through Stage 3 and end of run.
- /Users/RyanLandvater/Programming_Projects/FHIR_Testing/bench/arm_json_fhir.cpp
  Replace the placeholder Test 2 call with real DOM materialization and retained ownership plumbing.
- /Users/RyanLandvater/Programming_Projects/FHIR_Testing/bench/arm_hl7v2.cpp
  Replace the placeholder Test 2 call with real HL7 retained-tree materialization and retained ownership plumbing.
- /Users/RyanLandvater/Programming_Projects/FHIR_Testing/bench/bench_test_3.hpp
  Add query overloads/adapters that consume the retained tree wrappers instead of reparsing payloads.
- /Users/RyanLandvater/Programming_Projects/FHIR_Testing/bench/timing_conformance_test.cpp
  Update conformance expectations so real Test 2 work is required and Stage 3 does not rely on raw-payload reparsing.
- /Users/RyanLandvater/Programming_Projects/FHIR_Testing/bench/harness.hpp
  Adjust shared declarations only if a common retained-tree wrapper or helper type needs to be visible across arms.
- /Users/RyanLandvater/Programming_Projects/FHIR_Testing/bench/hl7v2_message.hpp
  Use as the semantic reference for HL7 segment/field structure when defining the retained HL7 tree.

**Verification**
1. Configure and build the harness and conformance targets successfully after replacing the placeholder Test 2 path.
2. Run `bench_timing_conformance` and verify all arms emit non-placeholder Test 2 timing events and Stage 3 does not reparse raw payloads.
3. Run a `bench_harness` smoke test and verify all three arms emit Stage 1/2/3 metrics with stable parity outputs.
4. Confirm the FastFHIR arm retains parser/tree ownership through the end of the arm run and that JSON/HL7 wrappers similarly retain their parse trees.
5. Inspect timed code paths for fairness guardrails: no FFHR-to-JSON bridge, no deferred HL7 line scanning in Stage 3, no hidden reparsing in JSON Stage 3.

**Decisions**
- Keep Test 2 implementation header-only unless a later constraint forces a split.
- Keep arm-native payload types at the Test 2 boundary: FastFHIR may take `FastFHIR::Memory`, JSON and HL7v2 may take payload strings.
- Use one shared Test 2 entry function pattern and one shared result-wrapper contract rather than forcing a single literal runtime type across all arms.
- Test 2 must eagerly touch and retain the full reachable in-memory tree until the end of the test.
- Stage 3 must query the retained materialized form, not the raw payload.

**Further Considerations**
1. FastFHIR retained-tree depth: retain only parser/root plus full eager walk, or also cache all visited nodes explicitly.
Recommendation: parser/root plus full eager walk unless Stage 3 needs pre-collected nodes.
2. HL7 retained-tree structure: use a generic segment/field tree or typed ORU structs.
Recommendation: generic segment/field tree first for parity and lower implementation risk.
3. Common wrapper design: use a templated wrapper or macro-selected struct aliases.
Recommendation: one common struct name with arm-local payload fields under macros to keep call sites uniform.
