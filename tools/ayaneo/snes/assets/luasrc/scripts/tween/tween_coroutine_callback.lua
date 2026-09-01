require("../core/core.lua")
require("../core/pool.lua")

TweenCoroutineCallbackNode = class()

createPooledNewAndFreeMethods(TweenCoroutineCallbackNode, true)

function TweenCoroutineCallbackNode.init(arg_1_0, arg_1_1, arg_1_2)
	arg_1_0.func = arg_1_1.func
	arg_1_0.argCount = arg_1_1.argCount
	arg_1_0.args = arg_1_1.args
	arg_1_0.firstUpdate = true
	arg_1_0.coroutine = false
end

function TweenCoroutineCallbackNode.getDuration(arg_2_0)
	return 0
end

function TweenCoroutineCallbackNode.reset(arg_3_0)
	arg_3_0.firstUpdate = true
	arg_3_0.coroutine = false
end

function TweenCoroutineCallbackNode.update(arg_4_0, arg_4_1)
	if arg_4_0.firstUpdate then
		arg_4_0.firstUpdate = false
		arg_4_0.coroutine = coroutine.create(arg_4_0.func)

		local var_4_0, var_4_1 = coroutine.resume(arg_4_0.coroutine, unpack(arg_4_0.args, 1, arg_4_0.argCount))

		if not var_4_0 then
			arg_4_0.coroutine = false

			print(var_4_1)

			return arg_4_1, true
		end
	end

	if coroutine.status(arg_4_0.coroutine) == "dead" then
		arg_4_0.coroutine = false

		return arg_4_1, true
	else
		return 0, false
	end
end
