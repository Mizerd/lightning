# Fedora COPR "make srpm" entry point. COPR runs, in a Fedora mock chroot
# with network access and as root:
#   make -f <clone>/.copr/Makefile srpm outdir=<dir> spec=<spec field>
# The crates are vendored HERE, so the RPM build itself needs no network.
top := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
outdir ?= $(CURDIR)

.PHONY: srpm
srpm:
	dnf -y install git-core cargo rpm-build tar xz gzip
	$(top)/packaging-ci/scripts/build-copr-srpm.sh "$(outdir)"
