# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# sea-mlir with the pinned solvers (z3, Bitwuzla, Yices) built from resources
# into libexec. The pins are those of docker/Dockerfile and docs/Release.md.
class SeaMlir < Formula
  desc "MLIR dialect, interpreter and VC generator for SeaHorn programs"
  homepage "https://github.com/seahorn/sea-mlir"
  # Release: the tag's tarball and its sha256. docker/release.py --homebrew
  # replaces url, sha256 and version with a local `git archive` tarball
  # (packaging/homebrew/README.md).
  url "https://github.com/seahorn/sea-mlir/archive/refs/tags/v0.6.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license all_of: [
    { "Apache-2.0" => { with: "LLVM-exception" } },
    "MIT",
  ]

  depends_on "boost" => :build
  depends_on "cmake" => :build
  depends_on "gperf" => :build
  depends_on "meson" => :build
  depends_on "ninja" => :build
  depends_on "pkgconf" => :build
  depends_on "python@3.15" => :build
  depends_on "seahorn/seahorn/sea-mlir-llvm" => :build
  # LGPL: dependencies, never bundled (docs/design/BinaryRelease.md)
  depends_on "gmp"
  depends_on "mpfr"

  on_linux do
    depends_on "autoconf" => :build
    depends_on "llvm@18" => :build
  end

  # z3 master bbd46c9 (reports 5.4.2): Z3_REVISION
  resource "z3" do
    url "https://github.com/Z3Prover/z3/archive/bbd46c9ebdd5955333682fe9f3cefc2b7c883e9c.tar.gz"
    sha256 "cff07556c6b7c33478c60044fc41308923b873dca24b3eccc34a9bfb5d4b9d48"
  end

  # Bitwuzla 0.9.1: BITWUZLA_TAG; CaDiCaL and SymFPU at the revisions of its
  # subprojects/*.wrap
  resource "bitwuzla" do
    url "https://github.com/bitwuzla/bitwuzla/archive/refs/tags/0.9.1.tar.gz"
    sha256 "42707f38900a20bb18108e426ba667560d1fd2ccce0d4f75aa60439b546488b4"
  end

  resource "cadical" do
    url "https://github.com/arminbiere/cadical/archive/refs/tags/rel-2.1.2.tar.gz"
    sha256 "292c2bb8d712d6d05fce3d3df63b922b8fa45e03974a79f7bae5bf68c284f131"
  end

  resource "symfpu" do
    url "https://github.com/martin-cs/symfpu/archive/40bdec00e99f8ea1b96c3dac0a05eed11c541639.tar.gz"
    sha256 "ba17877fbf0c851e113fddaab225152f1c0b2044429396b56b2f113832e36ce5"
  end

  # Yices 2.8.0 (BSD-3-Clause), without MCSAT: no LibPoly. YICES_TAG
  resource "yices" do
    url "https://github.com/SRI-CSL/yices2/archive/refs/tags/yices-2.8.0.tar.gz"
    sha256 "cb4c58c3508a5e67c156ba26182cd540af644fb7ba39b2873feebf73f21201b2"
  end

  # PyYAML for check-sea only (pure Python, not installed): `brew style`
  # asks for libyaml, which it does not use
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
    # Boost headers: lib/Support includes them without find_package (the
    # Docker builds have them in /usr/include)
    ENV.append "CXXFLAGS", "-isystem #{formula_opt_include("boost")}"
    # check-sea's environment (lit passes PATH and HOME): `python3` with
    # yaml (bench tests; a venv with the pure Python PyYAML resource), and a
    # git (tests that commit to scratch repositories) with an identity
    venv = buildpath/"test-venv"
    system formula_opt_bin("python@3.15")/"python3.15", "-m", "venv", "--without-pip", venv
    resource("pyyaml").stage do
      (Pathname.glob(venv/"lib/python3*/site-packages").first/"yaml").install Dir["lib/yaml/*"]
    end
    # Homebrew's git shim needs its own environment, which lit drops
    git = which("git", ORIGINAL_PATHS.join(File::PATH_SEPARATOR)) || odie("no git outside Homebrew's shims")
    (venv/"bin").install_symlink git
    ENV.prepend_path "PATH", venv/"bin"
    (Pathname(Dir.home)/".gitconfig").write "[user]\n\tname = sea-mlir build\n\temail = build@localhost\n"
    notices = share/"doc/sea-mlir/third-party"
    solvers = libexec
    # every solver library finds its siblings in libexec/lib
    ENV.append "LDFLAGS", "-Wl,-rpath,#{solvers}/lib"

    resource("z3").stage do
      system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
             "-DZ3_BUILD_LIBZ3_SHARED=ON", "-DZ3_BUILD_TEST_EXECUTABLES=OFF",
             "-DZ3_BUILD_PYTHON_BINDINGS=OFF",
             *std_cmake_args(install_prefix: solvers)
      system "cmake", "--build", "build", "--parallel", ENV.make_jobs.to_s
      system "cmake", "--install", "build"
      notices.install "LICENSE.txt" => "Z3-LICENSE.txt"
    end

    resource("bitwuzla").stage do
      # the wraps' sources, offline: CaDiCaL from the package cache (meson
      # checks its hash), SymFPU (a git wrap) unpacked with its overlay
      mkdir_p "subprojects/packagecache"
      cp resource("cadical").fetch, "subprojects/packagecache/cadical-2.1.2.tar.gz"
      resource("symfpu").stage(Pathname.pwd/"subprojects/symfpu")
      cp_r "subprojects/packagefiles/symfpu/.", "subprojects/symfpu"
      system "meson", "setup", "build", "--prefix=#{solvers}", "--libdir=#{solvers}/lib",
             "--buildtype=release", "--wrap-mode=nodownload", "-Ddefault_library=shared",
             "-Dtesting=disabled", "-Dunit_testing=disabled"
      system "meson", "compile", "-C", "build", "-j", ENV.make_jobs.to_s
      system "meson", "install", "-C", "build"
      notices.install "COPYING" => "Bitwuzla-COPYING.txt"
      notices.install Dir["subprojects/cadical-*/LICENSE"].first => "CaDiCaL-LICENSE.txt"
      notices.install "subprojects/symfpu/LICENSE-BSD" => "SymFPU-LICENSE-BSD.txt"
    end

    resource("yices").stage do
      system "autoconf"
      system "./configure", "--prefix=#{solvers}"
      system "make", "-j#{ENV.make_jobs}"
      system "make", "install"
      notices.install "LICENSE.txt" => "Yices-LICENSE.txt"
      notices.install "etc/NOTICES" => "Yices-NOTICES.txt"
      notices.install "LICENSES/CPython-3.4.0a3-LICENSE.txt" => "Yices-CPython-3.4.0a3-LICENSE.txt"
      notices.install "LICENSES/MIT.txt" => "Yices-MIT.txt"
    end

    llvm = Formula["seahorn/seahorn/sea-mlir-llvm"]
    pin = File.read("LLVM_REVISION")[/^commit: *(\h+)/, 1]
    built = (llvm.opt_prefix/"LLVM_REVISION").read[/^commit: *(\h+)/, 1]
    odie "sea-mlir-llvm is #{built}, LLVM_REVISION is #{pin}" if pin != built

    solver_rpath = rpath(target: solvers/"lib")
    args = %W[
      -DMLIR_DIR=#{llvm.opt_lib}/cmake/mlir
      -DLLVM_DIR=#{llvm.opt_lib}/cmake/llvm
      -DLLVM_EXTERNAL_LIT=#{llvm.opt_share}/lit/lit.py
      -DLLVM_LIT_ARGS=-sv\ -j#{ENV.make_jobs}
      -DZ3_ROOT=#{solvers}
      -DSEA_Z3=#{solvers}/bin/z3
      -DBITWUZLA_ROOT=#{solvers}
      -DSEA_ENABLE_YICES=ON
      -DYICES2_ROOT=#{solvers}
      -DSEA_GIT_DESCRIBE=v#{version}
      -DCMAKE_INSTALL_RPATH=#{solver_rpath}
    ]
    # no build path in the tools: assertion __FILE__ strings become
    # source-relative (Tools/no-source-paths.test)
    ENV.append "CFLAGS", "-ffile-prefix-map=#{buildpath}/="
    ENV.append "CXXFLAGS", "-ffile-prefix-map=#{buildpath}/="
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja", *args, *std_cmake_args
    system "cmake", "--build", "build", "--parallel", ENV.make_jobs.to_s
    # the tests of the release tarball's build (docker/Dockerfile)
    system "cmake", "--build", "build", "--target", "check-sea"
    system "cmake", "--install", "build"

    # As the tarball: no solver executables (SeaHorn's CMake needs
    # yices-smt2), no static libraries or package metadata
    rm(%w[z3 bitwuzla yices yices-sat yices-smt].map { |name| solvers/"bin"/name })
    rm Dir[solvers/"lib/*.a"]
    rm_r [solvers/"lib/cmake", solvers/"lib/pkgconfig"].select(&:exist?)
    # Yices installs its whole license directory (also the texts of
    # components not built here); its legal files are in third-party
    rm_r [solvers/"share/licenses", solvers/"share/doc"].select(&:exist?)
    (share/"doc/sea-mlir/LICENSES").install Dir["LICENSES/*"]
    cp llvm.opt_share/"doc/sea-mlir-llvm/LICENSE.TXT", notices/"LLVM-LICENSE.txt"
    prefix.install "LLVM_REVISION"
  end

  def caveats
    <<~EOS
      The documentation of this release, for people and agents:
        #{HOMEBREW_PREFIX}/share/doc/sea-mlir/docs
      Agents start at docs/agents/index.md there; `sea-vc --agent` prints it.
    EOS
  end

  test do
    version_text = shell_output("#{bin}/sea-vc --version")
    assert_match "sea-mlir", version_text
    assert_match(/^ *z3 5\.4\.2/, version_text)
    assert_match(/^ *yices 2\.8\.0$/, version_text)
    assert_match(/^ *bitwuzla 0\.9\.1$/, version_text)
    # docker/smoke/sat.mlir and unsat.mlir
    (testpath/"sat.mlir").write <<~MLIR
      sea.program @sat {
        %x = sea.nondet "value" [0] : i8
        %zero = sea.const 0 : i8
        %ok = sea.icmp eq, %x, %zero : i8
        sea.assert %ok
        sea.halt
      }
    MLIR
    (testpath/"unsat.mlir").write <<~MLIR
      !m = !sea.mem<!sea.ptr<64>, i8>
      sea.program @unsat {
        %zero = sea.const 0 : i8
        %memory = sea.mem.const %zero : !m
        %size = sea.const 16 : i64
        %object = sea.malloc %size align 16 [0] : i64 -> !sea.ptr<64>
        %value = sea.nondet "value" [1] : i8
        %written = sea.store %memory[%object], %value inplace : !m, i8
        %read = sea.load %written[%object] : !m -> i8
        %ok = sea.icmp eq, %read, %value : i8
        sea.assert %ok
        sea.halt
      }
    MLIR
    %w[z3 yices bitwuzla].each do |solver|
      sat = shell_output("#{bin}/sea-vc sat.mlir --lower --solver=#{solver}")
      assert_match(/^result: sat$/, sat)
      assert_match(/^replay: assert-failed/, sat)
      assert_match "mismatches=0", sat
    end
    assert_match(/^result: unsat$/, shell_output("#{bin}/sea-vc unsat.mlir --lower --config=seam"))
    assert_match(%r{^DOCS: #{Regexp.escape(HOMEBREW_PREFIX.to_s)}/.*share/doc/sea-mlir },
                 shell_output("#{bin}/sea-vc --agent"))
  end
end
