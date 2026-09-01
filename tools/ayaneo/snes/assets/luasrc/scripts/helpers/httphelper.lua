require("../core/core.lua")

HttpHelper = class()

function HttpHelper.httpInit(arg_1_0)
	arg_1_0.httpRequestsWaiting = {}
	arg_1_0.httpRequestsUniqueId = 0
end

function HttpHelper.httpDownload(arg_2_0, arg_2_1, arg_2_2)
	if arg_2_2 == nil then
		arg_2_2 = {}
	end

	local var_2_0 = coroutine.running()

	if not var_2_0 then
		error("Cannot call httpDownload from the main thread! You have to use it inside a coroutine.")
	end

	arg_2_0.httpRequestsWaiting[arg_2_0.httpRequestsUniqueId] = var_2_0

	HTTP.download(arg_2_1, arg_2_0._ptr, arg_2_0.httpRequestsUniqueId, arg_2_2)

	arg_2_0.httpRequestsUniqueId = arg_2_0.httpRequestsUniqueId + 1

	return coroutine.yield()
end

function HttpHelper.httpGet(arg_3_0, arg_3_1, arg_3_2, arg_3_3)
	if arg_3_2 == nil then
		arg_3_2 = {}
	end

	if arg_3_3 == nil then
		arg_3_3 = false
	end

	local var_3_0 = coroutine.running()

	if not var_3_0 then
		error("Cannot call httpGet from the main thread! You have to use it inside a coroutine.")
	end

	arg_3_0.httpRequestsWaiting[arg_3_0.httpRequestsUniqueId] = var_3_0

	HTTP.get(arg_3_1, arg_3_0._ptr, arg_3_0.httpRequestsUniqueId, arg_3_2, arg_3_3)

	arg_3_0.httpRequestsUniqueId = arg_3_0.httpRequestsUniqueId + 1

	return coroutine.yield()
end

function HttpHelper.httpPost(arg_4_0, arg_4_1, arg_4_2, arg_4_3, arg_4_4)
	if arg_4_3 == nil then
		arg_4_3 = {}
	end

	if arg_4_2 == nil then
		arg_4_2 = ""
	end

	if arg_4_4 == nil then
		arg_4_4 = false
	end

	local var_4_0 = coroutine.running()

	if not var_4_0 then
		error("Cannot call httpPost from the main thread! You have to use it inside a coroutine.")
	end

	arg_4_0.httpRequestsWaiting[arg_4_0.httpRequestsUniqueId] = var_4_0

	HTTP.post(arg_4_1, arg_4_0._ptr, arg_4_0.httpRequestsUniqueId, arg_4_3, arg_4_2, arg_4_4)

	arg_4_0.httpRequestsUniqueId = arg_4_0.httpRequestsUniqueId + 1

	return coroutine.yield()
end

function HttpHelper.onHttpRequestResult(arg_5_0, arg_5_1, arg_5_2, arg_5_3)
	local var_5_0, var_5_1 = coroutine.resume(arg_5_0.httpRequestsWaiting[arg_5_1], arg_5_3, arg_5_2)

	if var_5_0 == false then
		error(tostring(var_5_1))
	end

	arg_5_0.httpRequestsWaiting[arg_5_1] = nil
end
