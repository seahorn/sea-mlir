# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# LLVM + MLIR at the commit of sea-mlir's LLVM_REVISION, only to build
# sea-mlir (which links it statically). Bottled once per pin move.
class SeaMlirLlvm < Formula
  desc "LLVM and MLIR at the commit sea-mlir builds against"
  homepage "https://github.com/seahorn/sea-mlir"
  # LLVM_REVISION of sea-mlir; the version carries the pin
  url "https://github.com/llvm/llvm-project/archive/302c842d6601232c0fa75362404c82fb5ed1b433.tar.gz"
  version "24.0.0-git.302c842d6601"
  sha256 "7368cb6b6ee155a63bb967da2a1fce8c0868ae710358550ac3ae9b76def04fa3"
  license "Apache-2.0" => { with: "LLVM-exception" }

  keg_only "it is only used to build sea-mlir"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "python@3.15" => :build

  on_linux do
    depends_on "llvm@18" => :build
  end

  # Linux: clang 18 and lld (llvm@18), the toolchain of the tested builds
  # (docker/Dockerfile); with Homebrew's GCC 13, sea-mlir fails three
  # check-sea tests, and GNU ld 2.42 relaxes llvm@18's GOTPCRELX
  # relocations on `push` wrongly (seaopt crashed). The compiler is called directly,
  # not through Homebrew's shims, so the prefix's headers, libraries and
  # rpath are passed here.
  def use_clang18
    return unless OS.linux?

    llvm18 = Formula["llvm@18"]
    ENV["CC"] = (llvm18.opt_bin/"clang").to_s
    ENV["CXX"] = (llvm18.opt_bin/"clang++").to_s
    ENV["CPPFLAGS"] = "-isystem #{HOMEBREW_PREFIX}/include"
    ENV["LDFLAGS"] = "-fuse-ld=lld -L#{HOMEBREW_PREFIX}/lib -Wl,-rpath,#{HOMEBREW_PREFIX}/lib"
  end

  def install
    use_clang18
    commit = stable.url[%r{/archive/(\h{40})\.tar\.gz$}, 1]
    # As the release tarball's LLVM (docker/Dockerfile): Release with
    # assertions, the test utilities and lit installed. No optional system
    # libraries: sea-mlir links LLVM statically and needs none of them.
    args = %w[
      -DLLVM_ENABLE_PROJECTS=mlir
      -DLLVM_TARGETS_TO_BUILD=Native
      -DLLVM_ENABLE_ASSERTIONS=ON
      -DLLVM_INSTALL_UTILS=ON
      -DLLVM_INCLUDE_TESTS=OFF
      -DLLVM_INCLUDE_BENCHMARKS=OFF
      -DLLVM_INCLUDE_EXAMPLES=OFF
      -DLLVM_INCLUDE_DOCS=OFF
      -DLLVM_ENABLE_ZLIB=OFF
      -DLLVM_ENABLE_ZSTD=OFF
      -DLLVM_ENABLE_LZMA=OFF
      -DLLVM_ENABLE_LIBXML2=OFF
      -DLLVM_ENABLE_LIBEDIT=OFF
      -DLLVM_ENABLE_LIBPFM=OFF
      -DLLVM_ENABLE_FFI=OFF
      -DLLVM_ENABLE_CURL=OFF
      -DLLVM_ENABLE_HTTPLIB=OFF
    ]
    system "cmake", "-S", "llvm", "-B", "build", "-G", "Ninja", *args, *std_cmake_args
    system "cmake", "--build", "build", "--parallel", ENV.make_jobs.to_s
    system "cmake", "--install", "build"
    (share/"lit").install Dir["llvm/utils/lit/*"]
    (share/"doc/sea-mlir-llvm").install "llvm/LICENSE.TXT"
    (prefix/"LLVM_REVISION").write "commit: #{commit}\n"
  end

  test do
    assert_match "commit: #{version.to_s[/\h{12}$/]}", (prefix/"LLVM_REVISION").read
    assert_match "ON", shell_output("#{bin}/llvm-config --assertion-mode")
    assert_path_exists lib/"cmake/mlir/MLIRConfig.cmake"
    system bin/"FileCheck", "--version"
  end
end
