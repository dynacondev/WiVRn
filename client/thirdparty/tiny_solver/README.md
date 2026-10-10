# Vendored: Ceres tiny_solver.h

Single-header dense Levenberg-Marquardt solver for the fiducial board
pose fusion (`client/xr/board_solver.cpp`).

- Source: https://github.com/ceres-solver/ceres-solver/blob/master/include/ceres/tiny_solver.h
- Upstream rev at vendor time: `5ca5edf2a0b5` (master, 2026-10-10)
- License: BSD-3-Clause (header retained below; also copied to the
  client license assets, see `client/CMakeLists.txt`)
- Why vendored, not fetched: the header is explicitly standalone
  ("no dependencies beyond Eigen, including on other parts of Ceres,
  so it is possible to take this file alone"). Only `tiny_solver.h`
  is taken — the autodiff adapter (`tiny_solver_autodiff_function.h`)
  would drag in `jet.h` + 4 more Ceres headers, and is unneeded since
  analytic Jacobians come from manif.
- Refresh: re-download the file from the URL above, update the rev
  line, rebuild the client.
