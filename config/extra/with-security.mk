ifeq ($(FD_DISABLE_OPTIMIZATION),)
CPPFLAGS+=-fPIC -Wl,-z,now
else
CPPFLAGS+=-fPIC -Wl,-z,now
endif
LDFLAGS+=-fPIC
