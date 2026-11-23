QT = core network
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = identity-persistence-test
INCLUDEPATH += ../app
SOURCES += identity-persistence.cpp ../app/backend/identitymanager.cpp
LIBS += -lssl -lcrypto
