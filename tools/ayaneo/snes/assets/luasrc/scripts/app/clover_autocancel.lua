require("/scripts/app/clover_task.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_const.lua")

CloverAutoCancel = {
	Init = function()
		CloverAutoCancel.autoCancel = {}

		CloverTask.Register("auto_cancel", CloverAutoCancel.Update)
	end,
	Set = function(arg_2_0, arg_2_1, arg_2_2)
		local var_2_0 = CloverAutoCancel

		if not var_2_0.autoCancel then
			var_2_0.autoCancel = {}
		end

		local var_2_1 = var_2_0.autoCancel[arg_2_0] or {}

		var_2_1.container = arg_2_1
		var_2_1.count = CloverConst.System.AUTOCANCEL_SECONDS
		var_2_1.interrupt = false
		var_2_1.continuous = false
		var_2_1.continuousName = arg_2_2

		if arg_2_1 then
			var_2_0.autoCancelCurrentName = arg_2_0

			if arg_2_0 == var_2_0.autoCancelContinuousName then
				var_2_1.continuous = true
			else
				var_2_0.autoCancelContinuousName = nil
			end
		end

		var_2_0.autoCancel[arg_2_0] = var_2_1
	end,
	SetInterrupt = function(arg_3_0, arg_3_1)
		local var_3_0 = CloverAutoCancel

		if not var_3_0.autoCancel then
			var_3_0.autoCancel = {}
		end

		if not var_3_0.autoCancel[arg_3_0] then
			var_3_0.autoCancel[arg_3_0] = {}
		end

		var_3_0.autoCancel[arg_3_0].interrupt = arg_3_1
	end,
	ClearContinuous = function()
		CloverAutoCancel.autoCancelContinuousName = nil
	end,
	Update = function(arg_5_0)
		local var_5_0 = CloverAutoCancel

		if not var_5_0.autoCancel then
			return
		end

		local var_5_1 = var_5_0.autoCancel[var_5_0.autoCancelCurrentName]

		if not var_5_1 then
			return
		end

		if not var_5_1.container then
			return
		end

		local var_5_2 = false

		if var_5_1.container:isFocused() then
			var_5_2 = true
		elseif var_5_1.container:elementIsFocused() then
			var_5_2 = true
		end

		if not var_5_1.continuous then
			if CloverPadUI.pressed:isAnything() or var_5_1.interrupt or not var_5_2 then
				var_5_1.count = CloverConst.System.AUTOCANCEL_SECONDS

				return
			end

			if var_5_1.count > 0 then
				var_5_1.count = var_5_1.count - arg_5_0

				if var_5_1.count > 0 then
					return
				end
			end

			CloverPadUI:resetNoInputTime()

			var_5_0.autoCancelContinuousName = var_5_1.continuousName

			var_5_1.container:onContainerCanceled(nil)

			var_5_1.container = nil
		elseif var_5_2 then
			CloverPadUI:resetNoInputTime()

			var_5_0.autoCancelContinuousName = var_5_1.continuousName

			var_5_1.container:onContainerCanceled(nil)

			var_5_1.container = nil
		end
	end
}
