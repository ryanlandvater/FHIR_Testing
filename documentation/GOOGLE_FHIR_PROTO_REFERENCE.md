# Google FHIR C++ Library - Key Concepts and Reference

This document provides a summary of the key concepts and entry points for Google's C++ FHIR library, based on a review of the source code in the `google/fhir` repository. Unlike other libraries, the documentation is not centralized in a single README but is distributed across header files and build configurations.

## Overview

The Google FHIR C++ library is a protobuf-based implementation for working with FHIR resources. It is designed for performance and type safety, leveraging protocol buffers as the underlying data model. The library is built using the Bazel build system.

## Core Concepts

- **Protobuf-Based:** All FHIR resources and data types are defined as protocol buffer messages. This provides strong typing and efficient serialization/deserialization. The core proto definitions can be found in `proto/google/fhir/proto/`.

- **Primitive Handling:** The `PrimitiveHandler` class (`cc/google/fhir/primitive_handler.h`) provides a version-agnostic way to work with FHIR primitive types (e.g., `String`, `Decimal`, `DateTime`).

- **FHIR Packages:** The `FhirPackage` struct (`cc/google/fhir/fhir_package.h`) is used to load and manage collections of FHIR resources from a package (e.g., a `.zip` archive containing JSON representations of resources). It provides methods to access resources like `StructureDefinition`, `CodeSystem`, and `ValueSet` by their canonical URI.

- **Profiles and Extensions:** The library includes extensive support for working with FHIR profiles and extensions. The `profiles_lib.h` (`cc/google/fhir/profiles_lib.h`) and related files provide functionality for validating resources against profiles and handling extensions.

- **FHIRPath:** There is a C++ implementation of the FHIRPath specification for navigating and querying FHIR resources. The core logic is in `cc/google/fhir/fhir_path/`.

- **JSON Conversion:** The library provides tools for converting between the protobuf representation and FHIR JSON. See `cc/google/fhir/json/` for details.

## Key Source Files and Directories

- **`cc/google/fhir/`**: The root directory for the C++ source code.
  - **`primitive_handler.h`**: Defines the abstract interface for handling FHIR primitives.
  - **`fhir_package.h`**: Defines the `FhirPackage` for loading and accessing FHIR resources.
  - **`profiles_lib.h`**: Core library for handling FHIR profiles.
  - **`extensions.h`**: Utilities for working with extensions.
  - **`fhir_path/`**: FHIRPath implementation.
  - **`json/`**: Utilities for JSON conversion.
  - **`r4/`, `r5/`, `stu3/`**: Version-specific implementations and handlers.

- **`proto/google/fhir/proto/`**: Contains the protocol buffer definitions for FHIR resources and data types for different FHIR versions.

- **`BUILD` files**: Bazel build files throughout the repository define the structure, dependencies, and targets of the library. These are a valuable source of information for understanding how the different components fit together.

## Getting Started

To use the library, you will need to set up a Bazel workspace and declare dependencies on the required targets from the `google/fhir` repository. The `bazel/dependencies.bzl` file in the repository is a good starting point for understanding the external dependencies.

A typical workflow might involve:
1.  Loading a `FhirPackage` from a file archive.
2.  Using the package to retrieve `StructureDefinition`s or other conformance resources.
3.  Parsing JSON FHIR data into the corresponding protobuf messages.
4.  Using the C++ objects to access and manipulate FHIR data.
5.  Using FHIRPath to query the resources.
6.  Validating resources against profiles.
