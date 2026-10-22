# -*- coding: utf-8 -*-
"""build_ipk.py — 蜂助手面板 ipk 打包器（all-arch，纯 Python，无需 OpenWrt SDK）。

用法: py -3 build_ipk.py [版本号]
输入: luci-app-fzs/control/ (control, postinst, prerm, conffiles...)
      luci-app-fzs/data/    (文件树，按绝对路径落到目标系统)
输出: dist/luci-app-fzs_<ver>_all.ipk

格式（与 mifi-at_1.0-4_all.ipk 一致）:
  gzip( tar{ ./debian-binary, ./control.tar.gz, ./data.tar.gz } )
  control.tar.gz = ./control (+postinst/prerm)
  data.tar.gz    = ./data/ 的内容
"""
import gzip, io, os, sys, tarfile, time, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.join(HERE, "luci-app-fzs")
DIST = os.path.join(HERE, "dist")
# 文件时间戳必须用真实构建时间：设成 0(1970) 会让浏览器把 JS/CSS 长期缓存，更新后不生效
BUILD_TS = int(time.time())

def read_ctl(name):
    """读 control/ 下的控制文件。

    ★ 必须统一成 LF：仓库在 Windows 上 checkout 后这些文件常带 CRLF，
      打包时若原样塞进 ipk，设备上执行 postinst 会直接语法错
      （实测：ash 报 `line 3: { : not found`，因为 `{` 变成了 `{\\r`），
      后果是服务没被 enable/restart、LuCI 缓存没清 —— 装完看着成功实则没生效。
      control 里的 CRLF 同样危险：字段值会带上 \\r，可能影响 opkg 的版本比较。
    """
    p = os.path.join(PKG, "control", name)
    if not os.path.exists(p):
        return None
    return open(p, "rb").read().replace(b"\r\n", b"\n")

def get_version():
    ctl = read_ctl("control").decode("utf-8")
    for line in ctl.splitlines():
        if line.startswith("Version:"):
            return line.split(":", 1)[1].strip()
    return "0.0.0-1"

def make_tar_gz(files_spec, arc_prefix="."):
    """files_spec: [(arcname, bytes|None, mode)]  data=None 表示目录条目"""
    raw = io.BytesIO()
    t = tarfile.open(fileobj=raw, mode="w")
    for arcname, data, mode in files_spec:
        ti = tarfile.TarInfo(arcname)
        ti.mode = mode
        ti.mtime = BUILD_TS
        if data is None:
            ti.type = tarfile.DIRTYPE
            t.addfile(ti)
        else:
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    t.close()
    return gzip.compress(raw.getvalue(), 9, mtime=0)

EXEC_DIRS = ("/usr/libexec/", "/usr/bin/", "/usr/sbin/", "/etc/init.d/", "/etc/hotplug.d/")
TEXT_EXT = (".sh", ".js", ".json", ".htm", ".html", ".css", ".txt", ".conf")


def read_data(fp, arc):
    """读文件内容；文本文件统一成 LF。
    Windows 上写出的 CRLF 会把 #!/bin/sh 变成 #!/bin/sh\\r，脚本直接跑不起来。

    ★ 二进制文件（如内置的 fzs-atio ELF，它没有扩展名）绝不能被 CRLF 替换：
      那会把 ELF 里恰好出现的 0d0a 字节序列改坏，二进制直接报废。
      判据：内容含 NUL 字节即视为二进制。"""
    b = open(fp, "rb").read()
    name = os.path.basename(arc)
    if b"\x00" in b:
        return b
    if "." not in name or name.endswith(TEXT_EXT):
        b = b.replace(b"\r\n", b"\n")
    return b


def walk_data(droot):
    """返回 data.tar.gz 的条目：先目录后文件（opkg 不会自动创建中间目录）。"""
    out = []
    seen_dirs = set()
    for root, dirs, files in os.walk(droot):
        rel = os.path.relpath(root, droot).replace("\\", "/")
        if rel != ".":
            parts = rel.split("/")
            for i in range(len(parts)):
                d = "/".join(parts[:i + 1])
                if d not in seen_dirs:
                    seen_dirs.add(d)
                    out.append(("./" + d + "/", None, 0o755))
        for f in sorted(files):
            fp = os.path.join(root, f)
            arc = (rel + "/" + f).lstrip("./")
            path = "/" + arc
            mode = 0o755 if (path.startswith(EXEC_DIRS) or f.endswith(".sh")) else 0o644
            out.append(("./" + arc, read_data(fp, arc), mode))
    return out

def main():
    ver = sys.argv[1] if len(sys.argv) > 1 else get_version()
    ctl = read_ctl("control")
    if not ctl:
        sys.exit("control 文件不存在")
    ctl = ctl.replace(b"@VERSION@", ver.encode())
    postinst = read_ctl("postinst")
    prerm = read_ctl("prerm")

    ctl_files = [("./control", ctl, 0o644)]
    if postinst:
        ctl_files.append(("./postinst", postinst, 0o755))
    if prerm:
        ctl_files.append(("./prerm", prerm, 0o755))

    # ★ conffiles：把 data/etc/config/ 下的文件声明为「配置文件」。
    #   缺了它，opkg 会把 /etc/config/* 当普通文件，**升级时直接覆盖用户的配置**
    #   （本项目踩过：装新版本把用户的转发 token / 渠道设置冲回默认值）。
    #   只扫 etc/config/，不含 etc/init.d/ —— 服务脚本应该随版本更新。
    conf_dir = os.path.join(PKG, "data", "etc", "config")
    conf_entries = []
    if os.path.isdir(conf_dir):
        for _fn in sorted(os.listdir(conf_dir)):
            if os.path.isfile(os.path.join(conf_dir, _fn)):
                conf_entries.append("/etc/config/" + _fn)
    if conf_entries:
        ctl_files.append(("./conffiles", ("\n".join(conf_entries) + "\n").encode(), 0o644))

    data_dir = os.path.join(PKG, "data")
    data_files = walk_data(data_dir) if os.path.isdir(data_dir) else []
    if not data_files:
        print("警告: data/ 为空 —— 只打空包（骨架验证用）")

    control_tgz = make_tar_gz(ctl_files)
    data_tgz = make_tar_gz(data_files)

    inner = io.BytesIO()
    t = tarfile.open(fileobj=inner, mode="w")
    for name, content in [("./debian-binary", b"2.0\n"), ("./control.tar.gz", control_tgz), ("./data.tar.gz", data_tgz)]:
        ti = tarfile.TarInfo(name)
        ti.size = len(content)
        ti.mode = 0o644
        ti.mtime = BUILD_TS
        t.addfile(ti, io.BytesIO(content))
    t.close()
    outer = gzip.compress(inner.getvalue(), 9, mtime=0)

    os.makedirs(DIST, exist_ok=True)
    out = os.path.join(DIST, "luci-app-fzs_%s_all.ipk" % ver)
    open(out, "wb").write(outer)
    print("输出: %s (%d bytes) md5=%s" % (out, len(outer), hashlib.md5(outer).hexdigest()))

if __name__ == "__main__":
    main()
