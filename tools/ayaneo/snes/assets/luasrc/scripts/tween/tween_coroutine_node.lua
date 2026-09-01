require("../core/core.lua")
require("../core/pool.lua")

TweenResumeCoroutineNode = class()

createPooledNewAndFreeMethods(TweenResumeCoroutineNode, true)

function TweenResumeCoroutineNode.init(arg_1_0, arg_1_1, arg_1_2)
	arg_1_0.coroutineToResume = coroutine.running()
end

function TweenResumeCoroutineNode.getDuration(arg_2_0)
	return 0
end

function TweenResumeCoroutineNode.reset(arg_3_0)
	return
end

function TweenResumeCoroutineNode.update(arg_4_0, arg_4_1)
	coroutine.resume(arg_4_0.coroutineToResume)

	return arg_4_1, true
end
