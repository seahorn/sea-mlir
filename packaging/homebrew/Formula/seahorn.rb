# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# SeaHorn dev18 without clam (WITH_CLAM=OFF), at the pins of
# docker/SeaHorn.Dockerfile, as the export frontend of sea-mlir. Its z3 is
# 4.12.0 (a resource in libexec); its Yices is sea-mlir's libyices.so.2.8.
class Seahorn < Formula
  include Language::Python::Shebang

  desc "Verification framework for LLVM 18 programs, without Clam"
  homepage "https://github.com/seahorn/seahorn"
  # SEAHORN_REVISION; version 18.0.0-dev.<commit date> until SeaHorn releases
  url "https://github.com/seahorn/seahorn/archive/5373d0789f61d6cb0645641fe0e64e21366d12b3.tar.gz"
  version "18.0.0-dev.20261125"
  sha256 "adfd7750401a1a05d15dac989f2ac1030398c0011772e7a03168352f72293637"
  license all_of: [
    { "Apache-2.0" => { with: "LLVM-exception" } },
    "BSD-2-Clause",
    "BSL-1.0",
    "MIT",
    "NCSA",
  ]

  depends_on "boost" => :build
  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "gmp"
  # SeaHorn links LLVM 18 statically; `sea` runs its clang-18 and llvm-link
  depends_on "llvm@18"
  depends_on "python@3.15"
  # libyices.so.2.8 and the export flow's sea-vc
  depends_on "seahorn/seahorn/sea-mlir"
  # llvm@18's static libraries link zstd and terminfo (ncurses)
  depends_on "zstd"

  uses_from_macos "ncurses"

  # SEADSA_REVISION
  resource "sea-dsa" do
    url "https://github.com/seahorn/sea-dsa/archive/1e33f160e8c0b3f395176af019efd7f67f82a79b.tar.gz"
    sha256 "11b69fed7a46f7aa4fa19754ae7adeb29c25d2cbfb067227a9191f14170e89a4"
  end

  # LLVM_SEAHORN_REVISION
  resource "llvm-seahorn" do
    url "https://github.com/seahorn/llvm-seahorn/archive/92258148416984b19111a44b9a2f660f7c4132a6.tar.gz"
    sha256 "dea323f76387e704a8d4bf341ce98d19ab4e06fd292c4aeaec0e381d565a8c15"
  end

  # z3 4.12.0 (feda706, SEAHORN_Z3_REVISION): SeaHorn's Spacer and units
  # need it, not sea-mlir's z3 (docs/design/BinaryRelease.md, "One z3 or two")
  resource "z3" do
    url "https://github.com/Z3Prover/z3/archive/refs/tags/z3-4.12.0.tar.gz"
    sha256 "5f575f0a3950760436217da1cc1a714569b6d4f664a75bb6775876328cf0a580"
  end

  # PyYAML for `sea yama`: its pure Python modules only, as in the release
  # bundle, so no libyaml (`brew style` asks for it: the one known offense)
  resource "pyyaml" do
    url "https://files.pythonhosted.org/packages/76/b8/c0b780928e2fb1c87e9e375818c6f121da2398b071012d1747be300b83f4/pyyaml-6.0.4.tar.gz"
    sha256 "e24bf71af8a432d7808fabff134d38dfe16d3cbb41f2873bb4b1e42422de0a1a"
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
    # SeaHorn's CMake looks for `python3`
    ENV.prepend_path "PATH", formula_opt_libexec("python@3.15")/"bin"
    notices = share/"doc/seahorn/third-party"
    notices.mkpath
    resource("sea-dsa").stage buildpath/"sea-dsa"
    resource("llvm-seahorn").stage buildpath/"llvm-seahorn"
    # the license files of each repository at its pin
    legal = %w[LICENSE LICENSE.TXT license.txt NOTICE THIRD-PARTY-NOTICES]
    { "seahorn" => buildpath, "sea-dsa" => buildpath/"sea-dsa",
      "llvm-seahorn" => buildpath/"llvm-seahorn" }.each do |name, dir|
      legal.each do |file|
        cp dir/file, notices/"#{name}-#{file}" if (dir/file).exist?
      end
      cp_r dir/"LICENSES", notices/"#{name}-LICENSES"
    end

    z3 = libexec/"z3"
    resource("z3").stage do
      system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
             "-DZ3_BUILD_LIBZ3_SHARED=ON", "-DZ3_BUILD_TEST_EXECUTABLES=OFF",
             "-DZ3_BUILD_PYTHON_BINDINGS=OFF",
             # z3 4.12 asks for CMake < 3.5, which CMake 4 no longer accepts
             "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
             *std_cmake_args(install_prefix: z3)
      system "cmake", "--build", "build", "--parallel", ENV.make_jobs.to_s
      system "cmake", "--install", "build"
      notices.install "LICENSE.txt" => "z3-4.12-LICENSE.txt"
    end

    llvm = Formula["llvm@18"]
    yices = formula_opt_libexec("seahorn/seahorn/sea-mlir")
    args = %W[
      -DLLVM_DIR=#{llvm.opt_lib}/cmake/llvm
      -DZ3_ROOT=#{z3}
      -DYICES2_HOME=#{yices}
      -DWITH_CLAM=OFF
    ]
    # SeaHorn's own layout (bin/sea finds bin/../lib/seapy) in libexec
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja", *args,
           *std_cmake_args(install_prefix: libexec)
    odie "SeaHorn did not configure with sea-mlir's Yices" unless
      File.read("build/CMakeCache.txt").include?("YICES2_LIBRARY:FILEPATH=#{yices}/lib/libyices")
    units = %w[units_z3 units_type_checker units_hex_dump]
    system "cmake", "--build", "build", "--parallel", ENV.make_jobs.to_s, "--target", "install", *units
    units.each { |unit| system "build/units/#{unit}" }

    # As the release bundle (docker/build-seahorn-release.sh): no z3 or
    # Yices copies, no test corpus (inputs derived from GPL-2.0 Linux
    # sources), no static libraries
    # (libyices.so is a symlink to a file that is not copied)
    rm [libexec/"bin/z3", libexec/"bin/yices-smt2", *Dir[libexec/"lib/libyices*"]]
      .select { |f| File.symlink?(f) || File.exist?(f) }
    rm_r libexec/"share/seahorn/test" if (libexec/"share/seahorn/test").exist?
    rm Dir[libexec/"lib/*.a"]
    rm_r [z3/"bin", z3/"include", z3/"lib/cmake", z3/"lib/pkgconfig"].select(&:exist?)
    (share/"doc/seahorn").install Dir[libexec/"share/doc/seahorn/*"]
    rm_r libexec/"share/doc"

    resource("pyyaml").stage do
      (libexec/"lib/seapy").install "lib/yaml"
      notices.install "LICENSE" => "PyYAML-LICENSE.txt"
    end
    cp Dir[llvm.opt_prefix/"LICENSE.TXT"].first || odie("no LICENSE.TXT in llvm@18"),
       notices/"LLVM-18-LICENSE.txt"
    # Boost (headers only): BSL-1.0 needs no notice in object code; its
    # text is in seahorn-LICENSES/BSL-1.0.txt
    commit = ->(url) { url[%r{/archive/(\h{40})\.tar\.gz$}, 1] }
    pins = {
      "seahorn"      => commit.call(stable.url),
      "sea-dsa"      => commit.call(resource("sea-dsa").url),
      "llvm-seahorn" => commit.call(resource("llvm-seahorn").url),
      "clam"         => "off",
      "z3"           => resource("z3").version.to_s,
      "yices"        => Formula["seahorn/seahorn/sea-mlir"].resource("yices").version.to_s,
      "llvm"         => llvm.version.to_s,
    }
    (libexec/"share/seahorn/build-pins.txt").write pins.map { |key, value| "#{key}: #{value}\n" }.join

    rewrite_shebang detected_python_shebang, libexec/"bin/sea", libexec/"bin/stats.py"
    # `sea` compiles with clang-18 from PATH: llvm@18's first
    (bin/"sea").write_env_script libexec/"bin/sea", PATH: "#{llvm.opt_bin}:$PATH"
    %w[seahorn seapp seaopt seainspect seadsa].each do |tool|
      bin.install_symlink libexec/"bin"/tool
    end
  end

  test do
    sea_vc = formula_opt_bin("seahorn/seahorn/sea-mlir")/"sea-vc"
    # docker/smoke/sat.c and unsat.c
    (testpath/"sat.c").write <<~C
      extern unsigned nd(void);
      #include "seahorn/seahorn.h"
      int main(void) {
        unsigned value = nd();
        sassert(value == 0);
        return 0;
      }
    C
    (testpath/"unsat.c").write <<~C
      #include "seahorn/seahorn.h"
      extern unsigned nd(void);
      int main(void) {
        unsigned first = nd();
        unsigned second = nd();
        assume(first <= 100);
        assume(second <= 100);
        sassert(first + second <= 200);
        return 0;
      }
    C
    %w[sat unsat].each do |verdict|
      system bin/"sea", "yama", "-y", "sea-mlir-export", "-m64",
             "--horn-sea-mlir-out=#{verdict}.mlir", "#{verdict}.c"
      assert_match "sea.nondet", (testpath/"#{verdict}.mlir").read
      result = shell_output("#{sea_vc} #{verdict}.mlir --lower")
      assert_match(/^result: #{verdict}$/, result)
      if verdict == "sat"
        assert_match(/^replay: assert-failed/, result)
        assert_match "mismatches=0", result
      end
      # SeaHorn's BMC with its Yices backend (sea-mlir's libyices.so.2.8)
      # its exit status is not the verdict
      bmc = shell_output("#{bin}/sea bpf -m64 --bmc=mono --horn-bmc-solver=smt-y2 " \
                         "--log=bmc #{verdict}.c 2>&1 || true")
      assert_match "bmc using Yices2", bmc
      assert_match(/^#{verdict}$/, bmc)
    end
  end
end
