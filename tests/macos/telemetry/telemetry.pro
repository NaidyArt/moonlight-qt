QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = test-statstelemetry

INCLUDEPATH += ../../../app

SOURCES += \
    test-statstelemetry.cpp \
    ../../../app/streaming/video/statstelemetry.cpp

HEADERS += \
    ../../../app/streaming/video/statstelemetry.h
