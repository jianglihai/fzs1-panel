# -*- coding: utf-8 -*-
"""创建 GitHub Release 并上传 ipk 附件。
令牌复用 ../fzs1_decloud/.github_token（KEY=value 格式，绝不打印令牌本体）。
用法：python src/push_release.py <tag> <ipk 路径>
"""
import base64, hashlib, json, os, sys, urllib.request, urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
OLD = r"C:\Users\zzt\.zcode\workspace\default\fzs1_decloud"
OWNER, REPO = "zzt718", "fzs1-panel"

TAG = sys.argv[1] if len(sys.argv) > 1 else "v0.2.8"
IPK = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(HERE), "src", "dist", "luci-app-fzs_0.2.8_all.ipk")

BODY = """**luci-app-fzs（蜂助手面板）v0.2.8**

适用于 MIBOX-668M2 / 蜂助手 S1（MT7628 + EC200T + EC200N），社区 OpenWrt 22.03+（LuCI）。

## 下载
- 下方附件 `luci-app-fzs_0.2.8_all.ipk` —— 在 LuCI「系统 → 软件包 → 上传软件包」安装。

## 更新要点
- **后台负载大幅下降**：整机 CPU **14.1% → 3.7%**、每秒新建进程 **18.3 → 3.7**（单核 580MHz 实测）。
  未注册的模组自动降频（20 秒一轮；有卡的模组照旧 2 秒，**按模组独立判断**），每轮读取合为一次调用。
- **短信中心总开关**：不需要短信的用户可一键关闭，不再后台轮询、不再收短信，占用归零。
  只影响短信；锁频段 / 改串 / APN / 概览不受影响。
- **多主题适配**（Argon / Bootstrap 等）：修正卡片内边距与宽度、复选框对齐、卡片标题排版；
  「停用数据」「飞行模式」等主操作按钮改为醒目主色；短信页各卡片各自显示操作结果。
- 兼容新版社区固件（OpenWrt 23.05）。

完整功能与安装说明见 [README](https://github.com/zzt718/fzs1-panel#readme)。
"""


def token(fn, key):
    for l in open(os.path.join(OLD, fn), "r", encoding="utf-8", errors="ignore"):
        l = l.strip()
        if l.startswith(key + "="):
            return l.split("=", 1)[1].strip() or None
    return None


GH = token(".github_token", "GITHUB_TOKEN")
if not GH:
    raise SystemExit("找不到 GITHUB_TOKEN")

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # 绕开回环代理


def req(url, method="GET", body=None, ctype="application/json"):
    data = json.dumps(body).encode() if (body is not None and ctype == "application/json") else body
    r = urllib.request.Request(url, data=data, method=method)
    r.add_header("Authorization", "Bearer " + GH)
    r.add_header("Accept", "application/vnd.github+json")
    r.add_header("Content-Type", ctype)
    try:
        resp = opener.open(r, timeout=60)
        return resp.status, resp.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


# 若已有同名 release 则复用
st, r = req("https://api.github.com/repos/%s/%s/releases" % (OWNER, REPO))
rel = next((x for x in json.loads(r) if x["tag_name"] == TAG), None) if st == 200 else None

if not rel:
    st, r = req("https://api.github.com/repos/%s/%s/releases" % (OWNER, REPO), "POST",
                {"tag_name": TAG, "name": TAG, "body": BODY, "draft": False, "prerelease": False})
    if st != 201:
        raise SystemExit("建 release 失败: %s %s" % (st, r[:300]))
    rel = json.loads(r)
    print("release created: %s" % rel["html_url"])
else:
    print("release exists: %s" % rel["html_url"])

# 上传 / 更新附件
data = open(IPK, "rb").read()
name = os.path.basename(IPK)
for a in rel.get("assets", []):
    if a["name"] == name:
        print("asset 已存在且服务端持有，跳过上传（如需更新请删 release 重发）")
        print("md5 local :", hashlib.md5(data).hexdigest())
        sys.exit(0)

st, r = req(rel["upload_url"].split("{")[0] + "?name=" + name, "POST", data,
            "application/octet-stream")
if st != 201:
    raise SystemExit("上传附件失败: %s %s" % (st, r[:300]))
a = json.loads(r)
print("asset uploaded: %s (%d B, state=%s)" % (a["browser_download_url"], a["size"], a["state"]))
print("md5 local :", hashlib.md5(data).hexdigest())
