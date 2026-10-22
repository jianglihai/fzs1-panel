'use strict';
'require baseclass';
'require rpc';

/*
 * fzsexec —— 蜂助手面板专用的 ubus file.exec 封装。
 *
 * ==========================================================================
 * ★ 为什么不能直接沿用 LuCI 自带的 fs.exec()？（本模块存在的唯一理由）
 * ==========================================================================
 *
 * OpenWrt 23.05 的 rpcd（版本 c07ab2f，2023-07-01）src/file.c 开头有这么一段：
 *
 *     rpc_file_exec_run(cmd, sid, arg, env, ctx, req)
 *     {
 *         ...
 *         if (sid && env)
 *             return UBUS_STATUS_PERMISSION_DENIED;   // ← 第 812 行
 *
 * 即「只要请求同时携带 ubus_rpc_session 和 env，就无条件拒绝」，
 * 这个判定**发生在任何 ACL 检查之前**，所以 ACL 写得再全也没用。
 * （理由是：带自定义 env 时 rpcd 无法再按可信的命令行做 ACL 归属判断，于是直接禁掉。）
 *
 * 而 LuCI 的 fs.js 偏偏一定会带上 env：
 *
 *     callFileExec = rpc.declare({
 *         object: 'file',
 *         method: 'exec',
 *         params: [ 'command', 'params', 'env' ]     // ← 声明了 3 个参数
 *     });
 *
 *     exec: function(command, params, env) {
 *         if (!Array.isArray(params))  params = null;
 *         if (!L.isObject(env))        env = null;   // ← 没传 env 时置 null
 *         return callFileExec(command, params, env);
 *     }
 *
 * 致命处在 rpc.js 的 declare()：它把**每一个声明过的参数**无条件写进请求对象——
 *
 *     for (p_off = 0; p_off < options.params.length; p_off++)
 *         params[options.params[p_off]] = args[p_off];
 *
 * 于是即使调用方压根没传 env，JSON 里也必然出现 "env": null。
 * 键存在 ⇒ blobmsg 侧拿到的是个非 NULL 属性（RP​C_E_ENV policy 声明为
 * BLOBMSG_TYPE_TABLE），`sid && env` 成立 ⇒ **fs.exec() 一律 Permission denied**。
 *
 * 结论：在 OpenWrt 23.05 上，LuCI 的 fs.exec() 是对着 rpcd 的这个坑撞枪口，
 * 面板所有依赖它的 AT 交互（概览/APN/频段/小区/IMEI/短信）会集体取不到数据，
 * 表现为「卡片全空 / 点了没反应」——但设备上的服务其实一切正常。
 *
 * ==========================================================================
 * 修法
 * ==========================================================================
 * 本模块的 declare **故意只声明 ['command', 'params']**：
 * 少声明一个，rpc.js 的循环就到不了 env，请求里不会出现 env 键，
 * rpcd 侧 tb[RPC_E_ENV] == NULL，`sid && env` 不成立，ACL 随即正常生效。
 *
 * 返回值与 fs.exec() 完全一致（resolve 出 { code, stdout, stderr }），
 * 因此调用点可以原位替换，语义不变。
 */

var callExec = rpc.declare({
	object: 'file',
	method: 'exec',
	// ★ 不能加 'env' —— 加了就会重新踩进上面的坑。
	params: [ 'command', 'params' ]
});

return baseclass.extend({
	exec: function(command, params) {
		// params 必须是数组。
		// policy 里 params 声明为 BLOBMSG_TYPE_ARRAY；若这里留 null/undefined，
		// JSON.stringify 会把整个键丢掉，rpcd 侧 arg == NULL。后果是：
		// 当命令本体不在 ACL 白名单、需要走「整行拼接再匹配」的回退分支时
		// （file.c 里 arg == NULL 直接 return PERMISSION_DENIED），就再也没得匹配。
		// 统一补成空数组，行为才稳定。
		if (!Array.isArray(params))
			params = [];

		return callExec(command, params);
	}
});
