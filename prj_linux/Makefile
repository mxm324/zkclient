############################################################################
#   usage     :  make -f makefile [COMPILER=xxx] [DBG=X] [...]
#   parameter :  COMPILER: x86/82xx/85xx/arm,etc. default is x86
#                DBG: 0:means release; 1:means debug
#
#   history   :  author             date             modify reason
#               tuwenbin         2014-06-20          create                   
############################################################################
CPP=g++
LD=g++
RM=rm -f
INSTALL=install -D -m 644
AR=ar crus

CFLAGS=-D_LINUX_ -Wall
LDFLAGS=-shared

SRC_DIR:=../source
HEADER_DIR:=../include ../../../../10-common/include/zookeeper ../../../../10-common/include/zkclient

LIB_SHARED:=libzkclient.so
#LIB_STATIC:=libzkclient.a

LIBS := pthread zookeeper rt

LIBS_DBG := pthread zookeeper rt
		
LIB_PATH+= .
#DBG的值如果是1，则编译DEBUG版本
ifeq ($(DBG),0)                  
CFLAGS+=-O2
DST_DIR:=../../../../10-common/lib/release/linux/
LIB_PATH+= ../../../../10-common/lib/release/linux/
else
CFLAGS+=-D_DEBUG
DST_DIR:=../../../../10-common/lib/debug/linux/
LIB_PATH+= ../../../../10-common/lib/debug/linux/
endif

CFLAGS += -DTHREADED
CFLAGS += -DUSE_STATIC_LIB 

LDFLAGS += $(foreach path,$(LIB_PATH),-L$(path))

ifeq ($(DBG),0)                  
LDFLAGS += $(foreach lib,$(LIBS),-l$(lib)$(LIB_SUFFIX))
else
LDFLAGS += $(foreach lib,$(LIBS_DBG),-l$(lib)$(LIB_SUFFIX))
endif

SRC_EXT += $(foreach dir, $(SRC_DIR), $(wildcard $(dir)/*.cpp) )

SRC_EXT_C+=$(foreach dir, $(SRC_DIR), $(wildcard $(dir)/*.c))

OBJ=$(patsubst %.cpp,%.o, $(SRC_EXT))

OBJ_C=$(patsubst %.c,%.o, $(SRC_EXT_C))

CFLAGS+=$(foreach dir, $(HEADER_DIR), -I$(dir))

$(OBJ):%.o:%.cpp
	$(CPP) -o $@ -c $< $(CFLAGS)

$(OBJ_C):%.o:%.c
	$(CPP) -o $@ -c $< $(CFLAGS)

.PHONY: all

static:$(OBJ) $(OBJ_C)
	$(AR) $(LIB_STATIC) $^
	
shared:$(OBJ) $(OBJ_C)
	$(LD) $^ $(LDFLAGS) -o $(LIB_SHARED) 

install:$(LIB_SHARED) 
	$(INSTALL) $(LIB_SHARED) $(DST_DIR)/$(LIB_SHARED)

all:shared install
	$(RM) $(OBJ) $(OBJ_C) $(LIB_SHARED) 
	
clean:
	$(RM) $(OBJ) $(OBJ_C) $(LIB_SHARED) 
	-$(RM) $(DST_DIR)/$(LIB_SHARED) 