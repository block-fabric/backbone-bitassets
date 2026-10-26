# BitAssets (eCash slot 4) rule module and binaries.
add_library(bitassets_core STATIC
  src/types.cpp
  src/chain.cpp
  src/state.cpp
  src/fixture.cpp
  src/json.cpp)
target_include_directories(bitassets_core PUBLIC include)
target_link_libraries(bitassets_core PUBLIC bt_core)

if(BT_WITH_NET)
  add_library(bitassets_node STATIC src/wallet.cpp src/app.cpp src/appui.cpp)
  target_link_libraries(bitassets_node PUBLIC bitassets_core bt_net bt_appui)
  add_executable(bitassets src/main.cpp)
  target_link_libraries(bitassets PRIVATE bitassets_node)
  add_executable(bitassets-cli src/cli.cpp)
  target_link_libraries(bitassets-cli PRIVATE bitassets_core bt_net)
  # Fetch full blocks from a public betanet peer over their QUIC protocol (client only).
  add_executable(bitassets-fetch-blocks tools/fetch_blocks.cpp)
  target_link_libraries(bitassets-fetch-blocks PRIVATE bitassets_core bt_net)
endif()
