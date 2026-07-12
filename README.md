# TNA server source

This repository tracks source recovered from the authoritative production server.

- `grpc_service/`: unified gRPC service, protobuf API, and runtime helper sources.
- `ai_dialog_service/`: AI diagnosis and conversational-advice HTTP service.
- `Source/TNA/`: TNA core required by `grpc_service/CMakeLists.txt`.
- `algorithm_sources/OD_Estimate/`: OD estimation service and CLI sources.
- `algorithm_sources/transit_aon_new/`: current public-transit AON helper sources.
- `algorithm_sources/TripGeneration/`: trip generation/distribution service sources.
- `algorithm_sources/transit_pg/`: legacy transit helper wrapper sources.
- `algorithm_sources/transitCapTL/`: transit core required by the legacy wrapper.

Generated builds, runtime logs, virtual environments, secrets, database credentials,
backups, and compiled binaries are intentionally excluded.

