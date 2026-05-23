# pet_behavior v12 - walk/run guard

This package keeps the split behavior module structure and tunes RUN after comparing:

- colleague BLE full-speed run samples
- colleague BLE corridor walking samples

Main idea:

- RUN requires sustained high body motion, not just high gyro.
- Corridor walking can produce high gyro/d_roll during turns, leash pulls, and head movement, so gyro weight is reduced.
- RUN still uses 7 one-second windows and requires 5 RUN-like votes.

Replace `components/pet_behavior/` with this folder.
