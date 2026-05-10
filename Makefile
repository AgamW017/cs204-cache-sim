# PIN Tool Build Configuration
PINKIT := $(CURDIR)/pin_kit
CONFIG_ROOT := $(PINKIT)/source/tools/Config

export PINKIT
export PIN_ROOT := $(PINKIT)

include $(CONFIG_ROOT)/makefile.config

include $(CURDIR)/makefile.rules

.PHONY: all clean nopincc nopincc-%

all: pintool

nopincc:
	$(MAKE) -f $(CURDIR)/makefile.nopincc TARGET=$(TARGET)

nopincc-%:
	$(MAKE) -f $(CURDIR)/makefile.nopincc TARGET=$(TARGET) TOOL=$*

clean:
	rm -f $(TOOLS)
	$(MAKE) -f $(CURDIR)/makefile.nopincc TARGET=$(TARGET) clean
