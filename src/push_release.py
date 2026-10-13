# -*- coding: utf-8 -*-
"""创建 GitHub Release 并上传 ipk 附件。
令牌复用 ../fzs1_decloud/.github_token（KEY=value 格式，绝不打印令牌本体）。
用法：python src/push_release.py <tag> <ipk 路径>
"""
import base64, hashlib, json, os, sys, urllib.request, urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
OLD = r"C:\Users\zzt\.zcode\workspace\default\fzs1_decloud"
OWNER, REPO = "zzt718", "fzs1-panel"

TAG = sys.argv[1] if len(sys.argv) > 1 else "v0.2.9"
IPK = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(HERE), "src", "dist", "luci-app-fzs_0.2.9_all.ipk")

BODY = """**luci-app-fzs（蜂助手面板）v0.2.9**

适用于 MIBOX-668M2 / 蜂助手 S1（MT7628 + EC200T + EC200N），社区 OpenWrt 22.03+（LuCI）。

## 下载
- 下方附件 `luci-app-fzs_0.2.9_all.ipk` —— 在 LuCI「系统 → 软件包 → 上传软件包」安装。

## 更新要点

### 一、新增「无线网络」：2.4G 发射功率可调到硬件上限
- 「网络设置」页新增「无线网络」卡片，一个开关把 2.4G 发射功率由 **20 dBm 提到 23 dBm**（本机硬件上限）。
- 改动会让**无线断开重连**，所以先弹确认；卡片上直接显示**当前实际发射功率**。
- 改完程序会**自己等无线起来并核对功率**；**没到位就自动还原原设置** —— 不会"点一下、热点没了"。
- 附带「改 WiFi 名称和密码」入口。
- 说明：开源无线驱动默认遵守中国法规把 2.4G 限在 20 dBm；**即使开启，与厂商原厂固件相比仍有差距**。

### 二、页面与措辞整理
- 「上网设置」→「**网络设置**」。
- 取消页面最底的「高级」卡，其 MultiWAN 说明与入口**移到「上网方式」那一栏正下方**
  （原文写"本页的选项"，但实际只有「上网方式」用 MultiWAN 做的）。
- 短信中心总开关「开启短信收发」→「**启用短信功能**」（原来与「短信转发」只差一个字，容易看错）。

### 三、升级说明
- 升级**不会改动你的任何无线设置**，也不会自动开启这个开关。

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
