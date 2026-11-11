CLUNKYARD ?= npx --yes @clunkyard/cli@2

.PHONY: all setup doctor match replay clean
all:
	$(CLUNKYARD) build
setup:
	$(CLUNKYARD) setup
doctor:
	$(CLUNKYARD) doctor
match:
	$(CLUNKYARD) match
replay:
	$(CLUNKYARD) replay
clean:
	$(CLUNKYARD) clean
