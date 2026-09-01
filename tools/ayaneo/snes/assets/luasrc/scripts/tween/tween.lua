require("../core/core.lua")
require("tween_ease.lua")
require("tween_sequence_node.lua")
require("tween_parallel_node.lua")
require("tween_animate_node.lua")
require("tween_callback_node.lua")
require("tween_loop_node.lua")
require("tween_manager.lua")

Tween = class()
Tween.elems = {}

local function var_0_0(arg_1_0, arg_1_1)
	local var_1_0 = new(Tween)

	var_1_0.klass = TweenSequenceNode
	var_1_0.elems = {}

	for iter_1_0 = 1, #arg_1_0.elems do
		var_1_0.elems[iter_1_0] = arg_1_0.elems[iter_1_0]
	end

	var_1_0.elems[#var_1_0.elems + 1] = arg_1_1

	return var_1_0
end

function Tween.parallel(arg_2_0, ...)
	return var_0_0(arg_2_0, {
		klass = TweenParallelNode,
		elems = {
			...
		}
	})
end

function Tween.animate(arg_3_0, arg_3_1, arg_3_2, arg_3_3, arg_3_4, arg_3_5, ...)
	return var_0_0(arg_3_0, {
		klass = TweenAnimateNode,
		obj = arg_3_1,
		duration = arg_3_2,
		easeFunction = arg_3_5 or Ease.linear,
		easeFunctionArgCount = select("#", ...),
		easeFunctionArgs = {
			...
		},
		properties = arg_3_4,
		kind = arg_3_3
	})
end

function Tween.callback(arg_4_0, arg_4_1, ...)
	return var_0_0(arg_4_0, {
		klass = TweenCallbackNode,
		func = arg_4_1,
		argCount = select("#", ...),
		args = {
			...
		}
	})
end

function Tween.coroutineCallback(arg_5_0, arg_5_1, ...)
	return var_0_0(arg_5_0, {
		klass = TweenCoroutineCallbackNode,
		func = arg_5_1,
		argCount = select("#", ...),
		args = {
			...
		}
	})
end

function Tween.loop(arg_6_0, arg_6_1, arg_6_2)
	return var_0_0(arg_6_0, {
		klass = TweenLoopNode,
		loopCount = arg_6_1,
		tween = arg_6_2
	})
end

function Tween.sequence(arg_7_0, ...)
	local var_7_0 = {
		...
	}
	local var_7_1 = new(Tween)

	var_7_1.klass = TweenSequenceNode
	var_7_1.elems = {}

	for iter_7_0 = 1, #arg_7_0.elems do
		var_7_1.elems[iter_7_0] = arg_7_0.elems[iter_7_0]
	end

	for iter_7_1 = 1, #var_7_0 do
		for iter_7_2 = 1, #var_7_0[iter_7_1].elems do
			var_7_1.elems[#var_7_1.elems + 1] = var_7_0[iter_7_1].elems[iter_7_2]
		end
	end

	return var_7_1
end

function Tween.wait(arg_8_0, arg_8_1)
	return arg_8_0:animate(nil, arg_8_1)
end

function Tween.waitCondition(arg_9_0, arg_9_1, ...)
	return var_0_0(arg_9_0, {
		klass = TweenWaitConditionNode,
		conditionFunc = arg_9_1,
		argCount = select("#", ...),
		args = {
			...
		}
	})
end

function Tween.worldNodeEnabledFrom(arg_10_0, arg_10_1, arg_10_2, arg_10_3)
	return arg_10_0:animate(arg_10_1, arg_10_2, "from", {
		worldNodeEnabled = arg_10_3
	})
end

function Tween.componentEnabledFrom(arg_11_0, arg_11_1, arg_11_2, arg_11_3)
	return arg_11_0:animate(arg_11_1, arg_11_2, "from", {
		componentEnabled = arg_11_3
	})
end

function Tween.moveFrom(arg_12_0, arg_12_1, arg_12_2, arg_12_3, arg_12_4, arg_12_5, ...)
	return arg_12_0:animate(arg_12_1, arg_12_2, "from", {
		localPositionX = arg_12_3,
		localPositionY = arg_12_4
	}, arg_12_5, ...)
end

function Tween.scaleFrom(arg_13_0, arg_13_1, arg_13_2, arg_13_3, arg_13_4, arg_13_5, ...)
	return arg_13_0:animate(arg_13_1, arg_13_2, "from", {
		localScaleX = arg_13_3,
		localScaleY = arg_13_4
	}, arg_13_5, ...)
end

function Tween.rotateFrom(arg_14_0, arg_14_1, arg_14_2, arg_14_3, arg_14_4, ...)
	return arg_14_0:animate(arg_14_1, arg_14_2, "from", {
		localRotation = arg_14_3
	}, arg_14_4, ...)
end

function Tween.colorFrom(arg_15_0, arg_15_1, arg_15_2, arg_15_3, arg_15_4, arg_15_5, arg_15_6, arg_15_7, ...)
	return arg_15_0:animate(arg_15_1, arg_15_2, "from", {
		red = arg_15_3,
		green = arg_15_4,
		blue = arg_15_5,
		alpha = arg_15_6
	}, arg_15_7, ...)
end

function Tween.alphaFrom(arg_16_0, arg_16_1, arg_16_2, arg_16_3, arg_16_4, ...)
	return arg_16_0:animate(arg_16_1, arg_16_2, "from", {
		alpha = arg_16_3
	}, arg_16_4, ...)
end

function Tween.worldNodeEnabledTo(arg_17_0, arg_17_1, arg_17_2, arg_17_3)
	return arg_17_0:animate(arg_17_1, arg_17_2, "to", {
		worldNodeEnabled = arg_17_3
	})
end

function Tween.componentEnabledTo(arg_18_0, arg_18_1, arg_18_2, arg_18_3)
	return arg_18_0:animate(arg_18_1, arg_18_2, "to", {
		componentEnabled = arg_18_3
	})
end

function Tween.moveTo(arg_19_0, arg_19_1, arg_19_2, arg_19_3, arg_19_4, arg_19_5, ...)
	return arg_19_0:animate(arg_19_1, arg_19_2, "to", {
		localPositionX = arg_19_3,
		localPositionY = arg_19_4
	}, arg_19_5, ...)
end

function Tween.scaleTo(arg_20_0, arg_20_1, arg_20_2, arg_20_3, arg_20_4, arg_20_5, ...)
	return arg_20_0:animate(arg_20_1, arg_20_2, "to", {
		localScaleX = arg_20_3,
		localScaleY = arg_20_4
	}, arg_20_5, ...)
end

function Tween.rotateTo(arg_21_0, arg_21_1, arg_21_2, arg_21_3, arg_21_4, ...)
	return arg_21_0:animate(arg_21_1, arg_21_2, "to", {
		localRotation = arg_21_3
	}, arg_21_4, ...)
end

function Tween.colorTo(arg_22_0, arg_22_1, arg_22_2, arg_22_3, arg_22_4, arg_22_5, arg_22_6, arg_22_7, ...)
	return arg_22_0:animate(arg_22_1, arg_22_2, "to", {
		red = arg_22_3,
		green = arg_22_4,
		blue = arg_22_5,
		alpha = arg_22_6
	}, arg_22_7, ...)
end

function Tween.alphaTo(arg_23_0, arg_23_1, arg_23_2, arg_23_3, arg_23_4, ...)
	return arg_23_0:animate(arg_23_1, arg_23_2, "to", {
		alpha = arg_23_3
	}, arg_23_4, ...)
end

function Tween.moveBy(arg_24_0, arg_24_1, arg_24_2, arg_24_3, arg_24_4, arg_24_5, ...)
	return arg_24_0:animate(arg_24_1, arg_24_2, "by", {
		localPositionX = arg_24_3,
		localPositionY = arg_24_4
	}, arg_24_5, ...)
end

function Tween.scaleBy(arg_25_0, arg_25_1, arg_25_2, arg_25_3, arg_25_4, arg_25_5, ...)
	return arg_25_0:animate(arg_25_1, arg_25_2, "by", {
		localScaleX = arg_25_3,
		localScaleY = arg_25_4
	}, arg_25_5, ...)
end

function Tween.rotateBy(arg_26_0, arg_26_1, arg_26_2, arg_26_3, arg_26_4, ...)
	return arg_26_0:animate(arg_26_1, arg_26_2, "by", {
		localRotation = arg_26_3
	}, arg_26_4, ...)
end

function Tween.colorBy(arg_27_0, arg_27_1, arg_27_2, arg_27_3, arg_27_4, arg_27_5, arg_27_6, arg_27_7, ...)
	return arg_27_0:animate(arg_27_1, arg_27_2, "by", {
		red = arg_27_3,
		green = arg_27_4,
		blue = arg_27_5,
		alpha = arg_27_6
	}, arg_27_7, ...)
end

function Tween.alphaBy(arg_28_0, arg_28_1, arg_28_2, arg_28_3, arg_28_4, ...)
	return arg_28_0:animate(arg_28_1, arg_28_2, "by", {
		alpha = arg_28_3
	}, arg_28_4, ...)
end

function Tween.start(arg_29_0, arg_29_1)
	local var_29_0 = arg_29_0.klass.new(arg_29_0, arg_29_1)

	return (TweenManager:startTween(var_29_0))
end

function Tween.resumeCoroutine(arg_30_0)
	return var_0_0(arg_30_0, {
		klass = TweenResumeCoroutineNode
	})
end

function Tween.startCoroutine(arg_31_0, arg_31_1)
	local var_31_0 = arg_31_0:resumeCoroutine():start(arg_31_1)

	coroutine.yield()

	return var_31_0
end
