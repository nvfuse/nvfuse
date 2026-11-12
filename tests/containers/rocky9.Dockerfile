FROM rockylinux:9

RUN dnf -y install clang compiler-rt make python3 && dnf clean all

WORKDIR /src
CMD ["sh", "tests/containers/run-rocky9.sh"]
