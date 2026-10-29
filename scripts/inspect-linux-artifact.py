#!/usr/bin/env python3
"""Inspect the Linux x64 host artifact without executing it (binutils required)."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import tarfile
import tempfile

FLOOR = {"cpu": "x86-64-v2", "GLIBC": "2.38", "GLIBCXX": "3.4.32", "CXXABI": "1.3.13"}
NEEDED = {"libncursesw.so.6", "libtinfo.so.6", "libstdc++.so.6", "libgcc_s.so.1", "libm.so.6", "libc.so.6"}
# An allowlist, not an incomplete extension denylist. Unknown instructions fail
# closed for review. ENDBR64 is backwards-compatible NOP on non-CET CPUs.
BASE = set("""adc add and bsf bsr bswap bt btc btr bts call cbw cdq cdqe clc cld cmc cmp
cmpxchg cmpxchg8b cmpxchg16b cpuid cqo cwd cwde dec div enter hlt idiv imul in inc int int3
iret iretd iretq jmp lahf lea leave lfence lock lods lodsb lodsd lodsq lodsw loop loope loopne
mfence mov movabs movs movsb movsd movsq movsw movsx movsxd movzx mul neg nop not or out
pause pop popf popfq push pushf pushfq rcl rcr rdtsc ret retf rol ror sahf sal sar sbb
scas scasb scasd scasq scasw sfence shl shld shr shrd stc std stos stosb stosd stosq stosw sub
syscall test ud2 wait xadd xchg xlat xor endbr64
bsf bsr clflush fxrstor fxsave emms fwait
f2xm1 fabs fadd faddp fbld fbstp fchs fclex fcmovb fcmovbe fcmove fcmovnb fcmovnbe fcmovne
fcmovnu fcmovu fcom fcomi fcomip fcomp fcompp fcos fdecstp fdiv fdivp fdivr fdivrp ffree
fiadd ficom ficomp fidiv fidivr fild fimul fincstp fist fistp fisttp fisub fisubr fld fld1 fldcw
fldenv fldl2e fldl2t fldlg2 fldln2 fldpi fldz fmul fmulp fnclex fninit fnop fnsave fnstcw fnstenv
fnstsw fpatan fprem fprem1 fptan frndint frstor fscale fsin fsincos fsqrt fst fstp fsub fsubp
fsubr fsubrp ftst fucom fucomi fucomip fucomp fucompp fxam fxch fxtract fyl2x fyl2xp1
movd movq movdqa movdqu movaps movapd movups movupd movss movhps movhpd movlps movlpd
movhlps movlhps movmskps movmskpd movntdq movnti movntpd movntps movntq maskmovdqu maskmovq
ldmxcsr stmxcsr prefetchnta prefetcht0 prefetcht1 prefetcht2
addpd addps addsd addss andnpd andnps andpd andps cmppd cmpps cmpsd cmpss comisd comiss
cvtdq2pd cvtdq2ps cvtpd2dq cvtpd2pi cvtpd2ps cvtpi2pd cvtpi2ps cvtps2dq cvtps2pd cvtps2pi
cvtsd2si cvtsd2ss cvtsi2sd cvtsi2ss cvtss2sd cvtss2si cvttpd2dq cvttpd2pi cvttps2dq cvttps2pi
cvttsd2si cvttss2si divpd divps divsd divss maxpd maxps maxsd maxss minpd minps minsd minss
mulpd mulps mulsd mulss orpd orps packssdw packsswb packuswb paddb paddw paddd paddq paddsb
paddsw paddusb paddusw pand pandn pavgb pavgw pcmpeqb pcmpeqw pcmpeqd pcmpgtb pcmpgtw pcmpgtd
pextrw pinsrw pmaddwd pmaxsw pmaxub pminsw pminub pmovmskb pmulhuw pmulhw pmullw pmuludq
por psadbw pshufd pshufhw pshuflw pshufw pslld pslldq psllq psllw psrad psraw psrld psrldq
psrlq psrlw psubb psubw psubd psubq psubsb psubsw psubusb psubusw punpckhbw punpckhdq
punpckhqdq punpckhwd punpcklbw punpckldq punpcklqdq punpcklwd pxor rcpps rcpss rsqrtps rsqrtss
shufpd shufps sqrtpd sqrtps sqrtsd sqrtss subpd subps subsd subss ucomisd ucomiss unpckhpd
unpckhps unpcklpd unpcklps xorpd xorps""".split())
V2 = set("""addsubpd addsubps haddpd haddps hsubpd hsubps lddqu movddup movshdup movsldup
pabsb pabsw pabsd palignr phaddw phaddd phaddsw phsubw phsubd phsubsw pmaddubsw pmulhrsw
pshufb psignb psignw psignd blendpd blendps blendvpd blendvps dppd dpps extractps insertps
movntdqa mpsadbw packusdw pblendvb pblendw pcmpeqq pextrb pextrd pextrq phminposuw pinsrb
pinsrd pinsrq pmaxsb pmaxsd pmaxuw pmaxud pminsb pminsd pminuw pminud pmovsxbd pmovsxbq
pmovsxbw pmovsxdq pmovsxwd pmovsxwq pmovzxbd pmovzxbq pmovzxbw pmovzxdq pmovzxwd pmovzxwq
pmuldq pmulld ptest roundpd roundps roundsd roundss crc32 pcmpestri pcmpestrm pcmpistri
pcmpistrm pcmpgtq popcnt""".split())
CONDITIONS = "a ae b be c e g ge l le na nae nb nbe nc ne ng nge nl nle no np ns nz o p pe po s z".split()
ALLOWED = BASE | V2 | {p + c for p in ("j", "cmov", "set") for c in CONDITIONS}
PREFIXES = {"data16", "addr32", "cs", "ds", "es", "ss", "fs", "gs", "rep", "repe", "repz", "repne", "repnz", "lock", "bnd", "notrack", "rex.W", "rex"}


def command(*args: str) -> str:
    return subprocess.check_output(args, text=True, env={**os.environ, "LC_ALL": "C"})


def inspect(binary: pathlib.Path) -> dict:
    header = command("readelf", "-hW", str(binary))
    if "Advanced Micro Devices X86-64" not in header or "ELF64" not in header:
        raise ValueError("artifact must be ELF64 Linux x64")
    notes = command("readelf", "-nW", str(binary))
    if re.search(r"x86-64-v[3-9]", notes):
        raise ValueError("ELF GNU ISA property exceeds x86-64-v2")
    program = command("readelf", "-lW", str(binary))
    loader = re.search(r"Requesting program interpreter: (.*?)\]", program)
    if not loader or loader[1] != "/lib64/ld-linux-x86-64.so.2":
        raise ValueError("unexpected ELF loader")
    needed = sorted(re.findall(r"\(NEEDED\).*?\[(.*?)\]", command("readelf", "-dW", str(binary))))
    if not needed or set(needed) - NEEDED:
        raise ValueError(f"unexpected dynamic dependencies: {needed}")
    symbols = command("readelf", "--dyn-syms", "--wide", str(binary))
    versions = {}
    for family in ("GLIBC", "GLIBCXX", "CXXABI"):
        found = set(re.findall(r"@" + family + r"_([A-Za-z0-9_.]+)", symbols))
        if not found:
            raise ValueError(f"no {family} version evidence")
        for version in found:
            if not re.fullmatch(r"\d+(?:\.\d+)+", version) or tuple(map(int, version.split("."))) > tuple(map(int, FLOOR[family].split("."))):
                raise ValueError(f"{family}_{version} exceeds {family}_{FLOOR[family]}")
        versions[family] = max(found, key=lambda x: tuple(map(int, x.split("."))))
    instructions = set()
    disassembly = command("objdump", "-d", "-M", "intel", "--insn-width=16", str(binary))
    for line in disassembly.splitlines():
        match = re.match(r"\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} )+)\s+(.+)", line)
        if not match:
            continue
        encoding = match[1].split()
        # EVEX uses 0x62 in 64-bit mode, even with only xmm/ymm operands.
        # APX uses REX2 (0xd5). Neither extension belongs to x86-64-v2.
        while encoding and encoding[0] in {"26", "2e", "36", "3e", "64", "65", "66", "67", "f0", "f2", "f3"}:
            encoding.pop(0)
        if encoding and encoding[0] in {"62", "d5", "c4", "c5"}:
            raise ValueError(f"instruction encoding exceeds x86-64-v2: {line.strip()}")
        parts = match[2].split()
        while parts and (parts[0] in PREFIXES or parts[0].startswith("rex.")):
            parts.pop(0)
        if not parts:
            continue  # Padding prefix at the end of an objdump line.
        mnemonic = parts[0]
        instructions.add(mnemonic)
        if mnemonic not in ALLOWED or re.search(r"\b(?:zmm\d+|k[0-7])\b|\{(?:sae|[rz][nud]-sae)\}", match[2]):
            raise ValueError(f"instruction outside reviewed x86-64-v2 allowlist: {line.strip()}")
    if not instructions:
        raise ValueError("no instruction evidence")
    with binary.open("rb") as stream:
        binary_digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"floor": FLOOR, "symbol_versions": versions, "dynamic_dependencies": needed,
            "loader": loader[1], "binary_sha256": binary_digest,
            "instructions": sorted(instructions)}


def inspect_path(path: pathlib.Path) -> dict:
    if path.name.endswith(".tar.gz"):
        with tarfile.open(path, "r:gz") as archive, tempfile.TemporaryDirectory(prefix="ava-inspect-") as tmp:
            members = [m for m in archive.getmembers() if m.name.endswith("/bin/ava")]
            if len(members) != 1 or not members[0].isfile():
                raise ValueError("archive must contain one regular bin/ava")
            binary = pathlib.Path(tmp) / "ava"
            with archive.extractfile(members[0]) as source, binary.open("wb") as target:
                import shutil
                shutil.copyfileobj(source, target)
            return inspect(binary)
    return inspect(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        report = json.dumps(inspect_path(args.artifact), indent=2, sort_keys=True) + "\n"
        if args.output:
            args.output.write_text(report)
        print(report, end="")
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.exit(1, f"artifact inspection failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
