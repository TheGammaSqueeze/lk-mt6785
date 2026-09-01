require("/scripts/core/core.lua")

sys_sram_icon = class(WorldNode)

function sys_sram_icon.start(arg_1_0)
	print("sys_sram_icon:start()")

	arg_1_0.tween = nil
	arg_1_0.move_tween = nil

	arg_1_0:setMode("none")

	local var_1_0, var_1_1 = arg_1_0:getLocalPosition()

	arg_1_0.org_x = var_1_0
	arg_1_0.org_y = var_1_1
end

function sys_sram_icon.stop(arg_2_0)
	print("sys_sram_icon:stop()")
end

function sys_sram_icon.setMode(arg_3_0, arg_3_1)
	if arg_3_0.tween then
		tween_stop(arg_3_0.tween)
	end

	arg_3_0.on_sprite:setColor(1, 1, 1, 1)

	arg_3_0.mode = arg_3_1

	;({
		none = function()
			arg_3_0.on_sprite:setVisible(false)
			arg_3_0.off_sprite:setVisible(false)
			arg_3_0.warning_sprite:setVisible(false)
		end,
		off = function()
			arg_3_0.on_sprite:setVisible(false)
			arg_3_0.off_sprite:setVisible(true)
			arg_3_0.warning_sprite:setVisible(false)
		end,
		on = function()
			arg_3_0.on_sprite:setVisible(true)
			arg_3_0.off_sprite:setVisible(false)
			arg_3_0.warning_sprite:setVisible(false)
		end,
		warning = function()
			arg_3_0.on_sprite:setVisible(false)
			arg_3_0.off_sprite:setVisible(false)
			arg_3_0.warning_sprite:setVisible(true)
		end,
		resume = function()
			arg_3_0.on_sprite:setVisible(true)
			arg_3_0.off_sprite:setVisible(false)
			arg_3_0.on_sprite:setColor(1, 1, 0, 1)

			arg_3_0.tween = Tween:loop(math.huge, Tween:sequence(Tween:colorTo(arg_3_0.on_sprite, 0.5, 1, 1, 0, 1), Tween:wait(0.75), Tween:colorTo(arg_3_0.on_sprite, 0.75, 0.5, 0.5, 0, 1))):start()
		end,
		appeal = function()
			arg_3_0.on_sprite:setVisible(true)
			arg_3_0.off_sprite:setVisible(false)
			arg_3_0.on_sprite:setColor(1, 1, 1, 1)

			arg_3_0.tween = Tween:sequence(Tween:colorTo(arg_3_0.on_sprite, 0.125, 0.25, 1, 0.375, 1), Tween:wait(0.5), Tween:colorTo(arg_3_0.on_sprite, 0.25, 0.25, 1, 1, 1)):start()
		end
	})[arg_3_0.mode]()
end

function sys_sram_icon.resetMode(arg_10_0)
	arg_10_0:setMode(arg_10_0.mode)
end

function sys_sram_icon.getMode(arg_11_0)
	return arg_11_0.mode
end

function sys_sram_icon.moveFromTitle(arg_12_0)
	if arg_12_0.mode ~= "on" then
		return
	end

	arg_12_0:disable()

	if arg_12_0.move_tween then
		tween_stop(arg_12_0.move_tween)
	end

	local var_12_0, var_12_1 = arg_12_0:getWorldPosition()

	local function var_12_2(arg_13_0, arg_13_1, arg_13_2)
		arg_13_0:setZIndex(-8)
		arg_13_0:setLocalScale(0.125, 0.125)
		arg_13_0:setWorldPosition(arg_13_1, arg_13_2)
	end

	local function var_12_3(arg_14_0)
		arg_14_0:setZIndex(8)
	end

	local function var_12_4(arg_15_0)
		arg_15_0:setZIndex(5)
	end

	arg_12_0.move_tween = Tween:sequence(Tween:worldNodeEnabledTo(arg_12_0, 0.25, true), Tween:callback(var_12_2, arg_12_0, var_12_0 - 60, var_12_1 + 130), Tween:parallel(Tween:moveBy(arg_12_0, 0.125, 0, 120, Ease.outQuad), Tween:scaleTo(arg_12_0, 0.125, 1.75, 1.75)), Tween:callback(var_12_3, arg_12_0), Tween:wait(0.25), Tween:parallel(Tween:moveTo(arg_12_0, 0.375, arg_12_0.org_x, arg_12_0.org_y, Ease.inOutQuad), Tween:scaleTo(arg_12_0, 0.375, 1, 1)), Tween:callback(var_12_4, arg_12_0)):start()
end
