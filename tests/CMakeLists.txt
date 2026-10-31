find_package(Qt6 REQUIRED COMPONENTS Test)

# 纯逻辑单元测试：不依赖界面，offscreen 也能跑
function(rts_add_test name)
    qt_add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE Qt::Core Qt::Gui Qt::Test rts-common)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR})
    rts_enable_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

rts_add_test(test_plotprotocol test_plotprotocol.cpp)
rts_add_test(test_csvparser test_csvparser.cpp ${PROJECT_SOURCE_DIR}/csv/csvparser.cpp)

# 集成测试：PlotServer 与 PlotClient 在同一进程里通过真实的本地管道通信
qt_add_executable(test_plotroundtrip
    test_plotroundtrip.cpp
    ${PROJECT_SOURCE_DIR}/plot/server/plotserver.cpp
    ${PROJECT_SOURCE_DIR}/plot/server/plotserver.h
    ${PROJECT_SOURCE_DIR}/plot/viewer/plotclient.cpp
    ${PROJECT_SOURCE_DIR}/plot/viewer/plotclient.h
)
target_link_libraries(test_plotroundtrip PRIVATE Qt::Core Qt::Gui Qt::Network Qt::Test rts-common)
target_include_directories(test_plotroundtrip PRIVATE ${PROJECT_SOURCE_DIR})
rts_enable_warnings(test_plotroundtrip)
add_test(NAME test_plotroundtrip COMMAND test_plotroundtrip)

# 暂停画面冻结的回归测试（需要 QApplication，CI 里用 offscreen）
qt_add_executable(test_plotcanvas
    test_plotcanvas.cpp
    ${PROJECT_SOURCE_DIR}/plot/server/plotserver.cpp
    ${PROJECT_SOURCE_DIR}/plot/server/plotserver.h
    ${PROJECT_SOURCE_DIR}/plot/viewer/plotclient.cpp
    ${PROJECT_SOURCE_DIR}/plot/viewer/plotclient.h
    ${PROJECT_SOURCE_DIR}/plot/viewer/plotcanvas.cpp
    ${PROJECT_SOURCE_DIR}/plot/viewer/plotcanvas.h
)
target_link_libraries(test_plotcanvas PRIVATE Qt::Core Qt::Gui Qt::Widgets Qt::Network Qt::Test rts-common)
target_include_directories(test_plotcanvas PRIVATE ${PROJECT_SOURCE_DIR})
rts_enable_warnings(test_plotcanvas)
add_test(NAME test_plotcanvas COMMAND test_plotcanvas)
set_tests_properties(test_plotcanvas PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# CSV 行列表：增删行保留已有值、命中测试（需要 QApplication，offscreen）
qt_add_executable(test_csvrowlist
    test_csvrowlist.cpp
    ${PROJECT_SOURCE_DIR}/csv/csvrowlist.cpp
    ${PROJECT_SOURCE_DIR}/csv/csvrowlist.h
    ${PROJECT_SOURCE_DIR}/csv/csvparser.cpp
)
target_link_libraries(test_csvrowlist PRIVATE Qt::Core Qt::Gui Qt::Widgets Qt::Test rts-common)
target_include_directories(test_csvrowlist PRIVATE ${PROJECT_SOURCE_DIR})
rts_enable_warnings(test_csvrowlist)
add_test(NAME test_csvrowlist COMMAND test_csvrowlist)
set_tests_properties(test_csvrowlist PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 版本比较与 Release 解析（不发网络请求）
qt_add_executable(test_updatechecker
    test_updatechecker.cpp
    ${PROJECT_SOURCE_DIR}/app/updatechecker.cpp
    ${PROJECT_SOURCE_DIR}/app/updatechecker.h
)
target_link_libraries(test_updatechecker PRIVATE Qt::Core Qt::Network Qt::Test)
target_include_directories(test_updatechecker PRIVATE ${PROJECT_SOURCE_DIR})
rts_enable_warnings(test_updatechecker)
add_test(NAME test_updatechecker COMMAND test_updatechecker)
