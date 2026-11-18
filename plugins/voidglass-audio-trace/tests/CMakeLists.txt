cmake_minimum_required(VERSION 3.28...3.30)
project(voidglass-audio-trace-tests LANGUAGES C)

enable_testing()
add_executable(audio-trace-tests audio-trace-tests.c)
target_compile_features(audio-trace-tests PRIVATE c_std_11)
add_test(NAME audio-trace-ownership-and-loss COMMAND audio-trace-tests)
