# GCP Terraform Scaffold

This folder will host Terraform modules/environments for private benchmark deployment.

Required outcomes:
- Sender and receiver compute instances.
- Private managed database (Cloud SQL) in same VPC/subnet domain as runners.
- No benchmark-path DB traffic via public internet.
- Outputs proving private endpoint locality.
