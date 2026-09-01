require("../core/core.lua")

TweenManager = class()

function TweenManager.start(arg_1_0)
	if TweenManager.manager ~= nil then
		error("Trying to start a TweenManager, but one already exists ('" .. TweenManager.manager.worldNode:getName() .. "')")
	end

	TweenManager.manager = arg_1_0

	TweenManager:init()
end

function TweenManager.stop(arg_2_0)
	if TweenManager.manager == arg_2_0 then
		TweenManager.manager = nil
	end

	TweenManager:shutdown()
end

function TweenManager.init(arg_3_0)
	if not arg_3_0.isInitialized then
		arg_3_0.activeTweens = {}
		arg_3_0.lastId = 0
		arg_3_0.globalSpeed = 1
		arg_3_0.isInitialized = true
	end
end

function TweenManager.shutdown(arg_4_0)
	if arg_4_0.isInitialized then
		arg_4_0.activeTweens = {}
		arg_4_0.isInitialized = false
	end
end

function TweenManager.registerProperty(arg_5_0, arg_5_1, arg_5_2, arg_5_3)
	addTweenPropertyAccessor(arg_5_1, arg_5_2, arg_5_3)
end

function TweenManager.startTween(arg_6_0, arg_6_1)
	arg_6_0:init()

	local var_6_0 = arg_6_0.lastId

	arg_6_0.lastId = var_6_0 + 1

	arg_6_1:reset()

	arg_6_1.id = var_6_0
	arg_6_0.activeTweens[#arg_6_0.activeTweens + 1] = arg_6_1

	return var_6_0
end

function TweenManager.stopTween(arg_7_0, arg_7_1)
	local var_7_0 = arg_7_0.activeTweens
	local var_7_1 = #var_7_0

	for iter_7_0 = 1, var_7_1 do
		if var_7_0[iter_7_0].id == arg_7_1 then
			var_7_0[iter_7_0].dead = true

			return
		end
	end
end

function TweenManager.isTweenActive(arg_8_0, arg_8_1)
	local var_8_0 = arg_8_0.activeTweens
	local var_8_1 = #var_8_0

	for iter_8_0 = 1, var_8_1 do
		if var_8_0[iter_8_0].id == arg_8_1 and not var_8_0[iter_8_0].isDead then
			return true
		end
	end

	return false
end

function TweenManager.update(arg_9_0, arg_9_1)
	arg_9_1 = arg_9_1 * arg_9_0.globalSpeed

	local var_9_0 = arg_9_0.activeTweens
	local var_9_1 = 1

	while var_9_1 <= #var_9_0 do
		local var_9_2 = var_9_0[var_9_1]
		local var_9_3 = var_9_2.dead

		if not var_9_3 then
			local var_9_4, var_9_5 = var_9_2:update(arg_9_1)

			var_9_3 = var_9_5
		end

		if var_9_3 then
			var_9_2.dead = false

			var_9_2:free()

			var_9_0[var_9_1] = var_9_0[#var_9_0]
			var_9_0[#var_9_0] = nil
		else
			var_9_1 = var_9_1 + 1
		end
	end
end
