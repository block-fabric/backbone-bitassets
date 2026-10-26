add_executable(bt_tests
  test_bitassets_types.cpp
  test_bitassets_replay.cpp
  test_bitassets_oracle.cpp)
target_link_libraries(bt_tests PRIVATE bt_test_harness bt_core_test_cases bitassets_core bt_core)
target_compile_definitions(bt_tests PRIVATE BT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME bt_tests COMMAND bt_tests)
if(BT_WITH_NET)
  target_sources(bt_tests PRIVATE test_bitassets_node.cpp)
  target_link_libraries(bt_tests PRIVATE bitassets_node bt_net)
  # The app layer (doc/app.md): the LayerTwo RPC the same with it, its reads, the phrase's flows, the spends described on
  # a chain built here, the manifest.
  target_sources(bt_tests PRIVATE test_bitassets_appui.cpp)
  target_compile_definitions(bt_tests PRIVATE BT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
