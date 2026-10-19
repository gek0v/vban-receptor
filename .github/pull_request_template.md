## Description
Provide a concise explanation of the changes introduced by this pull request, including the problem being solved or feature being added.

---

## Type of Change
- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to not work as expected)
- [ ] Performance improvement / DSP optimization
- [ ] Documentation update

---

## Pre-Submission Verification Checklist
Please verify all items before submitting:
- [ ] My code adheres to the style guidelines of this project (Kotlin / modern C++20).
- [ ] **Real-time safety**: I have ensured NO dynamic allocations, mutexes, or syscalls are performed on the audio callback path.
- [ ] I have executed the C++ native tests locally (`ctest`) and all tests pass.
- [ ] I have executed the Python E2E verification suite (`python e2e_tests/run_tests.py`) and all 84 tests pass.
- [ ] I have compiled the Android project (`./gradlew testDebugUnitTest assembleDebug`) and it builds without errors.
- [ ] I have updated relevant documentation where necessary.
