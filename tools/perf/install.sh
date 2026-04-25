sudo apt update
sudo apt install -y \
    libtraceevent-dev libtracefs-dev \
    libdw-dev libdebuginfod-dev \
    libelf-dev \
    libcapstone-dev \
    llvm-dev clang \
    liblzma-dev \
    libpfm4-dev \
    systemtap-sdt-dev \
    libslang2-dev \
    libbabeltrace-dev \
    libunwind-dev \
    libzstd-dev \
    libssl-dev \
    libnuma-dev \
    libperl-dev python3-dev \
    libcap-dev \
    binutils-dev libiberty-dev \
    default-jdk \
    rustc cargo

make -j$(nproc)
sudo make install
