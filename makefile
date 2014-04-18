#

CC = g++
OPTS = -Wall -pedantic -s -static -O2 $(OS_SPECIFIC_OPTS)
ifeq ($(OS),Windows_NT)
LIBS_BOOST =
LIBS_OS_SPECIFIC = -lws2_32 -lwsock32
LIBS_PATHS =
OS_SPECIFIC_OPTS = -DWIN32 -D_WIN32_WINNT=0x501
TARGET = async_loader.exe
else
LIBS_BOOST =
LIBS_OS_SPECIFIC =
LIBS_PATHS =
OS_SPECIFIC_OPTS = -DBOOST_THREAD_USE_LIB
TARGET = async_loader
endif
LIBS = $(LIBS_PATHS) $(LIBS_BOOST) $(LIBS_OS_SPECIFIC)


$(TARGET): async_loader.cpp
	$(CC) $(OPTS) -o $(TARGET) $< $(LIBS) 
