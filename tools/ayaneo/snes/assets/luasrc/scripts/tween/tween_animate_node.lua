require("../core/core.lua")
require("../core/pool.lua")

local var_0_0 = {
	worldNodeEnabled = {
		get = WorldNode.isEnabled,
		set = WorldNode.setEnabled
	},
	componentEnabled = {
		get = Component.isEnabled,
		set = Component.setEnabled
	},
	localPositionX = {
		get = WorldNode.getLocalPositionX,
		set = function(arg_1_0, arg_1_1)
			WorldNode.setLocalPositionX(arg_1_0, math.floor(arg_1_1 + 0.5))
		end
	},
	localPositionY = {
		get = WorldNode.getLocalPositionY,
		set = function(arg_2_0, arg_2_1)
			WorldNode.setLocalPositionY(arg_2_0, math.floor(arg_2_1 + 0.5))
		end
	},
	localScaleX = {
		get = WorldNode.getLocalScaleX,
		set = WorldNode.setLocalScaleX
	},
	localScaleY = {
		get = WorldNode.getLocalScaleY,
		set = WorldNode.setLocalScaleY
	},
	localRotation = {
		get = WorldNode.getLocalRotation,
		set = WorldNode.setLocalRotation
	},
	red = {
		get = VisualComponent.getColorR,
		set = VisualComponent.setColorR
	},
	green = {
		get = VisualComponent.getColorG,
		set = VisualComponent.setColorG
	},
	blue = {
		get = VisualComponent.getColorB,
		set = VisualComponent.setColorB
	},
	alpha = {
		get = VisualComponent.getAlpha,
		set = VisualComponent.setAlpha
	}
}

function addTweenPropertyAccessor(arg_3_0, arg_3_1, arg_3_2)
	var_0_0[arg_3_0] = {
		get = arg_3_1,
		set = arg_3_2
	}
end

TweenAnimateNode = class()

createPooledNewAndFreeMethods(TweenAnimateNode, true)

local var_0_1 = new(Pool)
local var_0_2 = new(Pool)

function TweenAnimateNode.init(arg_4_0, arg_4_1, arg_4_2)
	arg_4_0.currentTime = 0

	if type(arg_4_1.obj) == "string" then
		arg_4_0.obj = arg_4_2[arg_4_1.obj]
	else
		arg_4_0.obj = arg_4_1.obj
	end

	arg_4_0.duration = arg_4_1.duration
	arg_4_0.easeFunction = arg_4_1.easeFunction
	arg_4_0.easeFunctionArgCount = arg_4_1.easeFunctionArgCount
	arg_4_0.easeFunctionArgs = arg_4_1.easeFunctionArgs
	arg_4_0.kind = arg_4_1.kind
	arg_4_0.properties = var_0_1:alloc()

	if arg_4_1.properties then
		for iter_4_0, iter_4_1 in pairs(arg_4_1.properties) do
			if type(iter_4_1) == "string" then
				iter_4_1 = arg_4_2[iter_4_1]
			end

			local var_4_0 = var_0_2:alloc()

			var_4_0.get = var_0_0[iter_4_0].get
			var_4_0.set = var_0_0[iter_4_0].set
			var_4_0.targetValue = iter_4_1
			arg_4_0.properties[#arg_4_0.properties + 1] = var_4_0
		end
	end
end

function TweenAnimateNode.destroy(arg_5_0)
	for iter_5_0 = 1, #arg_5_0.properties do
		var_0_2:free(arg_5_0.properties[iter_5_0])
	end

	var_0_1:free(arg_5_0.properties)
end

function TweenAnimateNode.getDuration(arg_6_0)
	return arg_6_0.duration
end

function TweenAnimateNode.reset(arg_7_0)
	arg_7_0.currentTime = 0

	if arg_7_0.obj then
		for iter_7_0 = 1, #arg_7_0.properties do
			local var_7_0 = arg_7_0.properties[iter_7_0]

			if arg_7_0.kind == "to" then
				var_7_0.begValue = var_7_0.get(arg_7_0.obj)
				var_7_0.endValue = var_7_0.targetValue
			elseif arg_7_0.kind == "from" then
				var_7_0.begValue = var_7_0.targetValue
				var_7_0.endValue = var_7_0.get(arg_7_0.obj)
			elseif arg_7_0.kind == "by" then
				var_7_0.begValue = var_7_0.get(arg_7_0.obj)

				if type(var_7_0.begValue) == "number" then
					var_7_0.endValue = var_7_0.begValue + var_7_0.targetValue
				else
					var_7_0.endValue = var_7_0.targetValue
				end
			end
		end
	end
end

function TweenAnimateNode.update(arg_8_0, arg_8_1)
	if arg_8_0.currentTime <= arg_8_0.duration then
		arg_8_0.currentTime = arg_8_0.currentTime + arg_8_1

		if arg_8_0.obj then
			local var_8_0 = math.min(arg_8_0.currentTime, arg_8_0.duration)

			for iter_8_0 = 1, #arg_8_0.properties do
				local var_8_1 = arg_8_0.properties[iter_8_0]

				if type(var_8_1.begValue) == "number" then
					if arg_8_0.duration > 0 then
						var_8_1.set(arg_8_0.obj, arg_8_0.easeFunction(var_8_0, var_8_1.begValue, var_8_1.endValue - var_8_1.begValue, arg_8_0.duration, unpack(arg_8_0.easeFunctionArgs, 1, arg_8_0.easeFunctionArgCount)))
					else
						var_8_1.set(arg_8_0.obj, arg_8_0.easeFunction(1, var_8_1.begValue, var_8_1.endValue - var_8_1.begValue, 1, unpack(arg_8_0.easeFunctionArgs, 1, arg_8_0.easeFunctionArgCount)))
					end
				elseif arg_8_0.currentTime < arg_8_0.duration then
					var_8_1.set(arg_8_0.obj, var_8_1.begValue)
				else
					var_8_1.set(arg_8_0.obj, var_8_1.endValue)
				end
			end
		end

		return math.max(0, arg_8_0.currentTime - arg_8_0.duration), arg_8_0.currentTime > arg_8_0.duration
	else
		return 0, true
	end
end
