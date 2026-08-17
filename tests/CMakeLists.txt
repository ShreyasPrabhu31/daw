add_executable(daw_tests test_engine.cpp)
target_link_libraries(daw_tests PRIVATE daw_engine)
add_test(NAME daw_tests COMMAND daw_tests)
