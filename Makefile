CLUNKYARD ?= npx --yes @clunkyard/cli

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
