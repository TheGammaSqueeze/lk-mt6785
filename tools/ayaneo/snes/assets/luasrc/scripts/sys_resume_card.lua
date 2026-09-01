require("/scripts/core/core.lua")
require("/scripts/helper_tween.lua")
require("/scripts/Store_myplay_data.lua")
require("/scripts/app/clover_const.lua")
require("/scripts/app/clover_util.lua")

sys_resume_card = class(GUIButton)

function sys_resume_card.start(arg_1_0)
	GUIButton.start(arg_1_0)

	if arg_1_0.active_component then
		arg_1_0.active_component:disable()
	end

	if arg_1_0.inactive_component then
		arg_1_0.inactive_component:enable()
	end

	if arg_1_0.floating_component then
		arg_1_0.floating_component:disable()
	end

	if not REED_DEBUG and arg_1_0.debug_filelabel then
		arg_1_0.debug_filelabel:disable()
	end

	arg_1_0.dummytexture = arg_1_0.screen:getTexture()
	arg_1_0.is_floating = false
	arg_1_0.is_need_warning = false

	arg_1_0.screen:setColor(0, 0, 0, 1)
end

function sys_resume_card.activate(arg_2_0)
	if arg_2_0.activeAnimation then
		arg_2_0.activeAnimation:run(arg_2_0)
	end

	if arg_2_0.active_component then
		arg_2_0.active_component:enable()
	end

	if arg_2_0.inactive_component then
		arg_2_0.inactive_component:disable()
	end

	if system.cursor and arg_2_0.cursor_component then
		system.cursor:setRounded(arg_2_0.cursor_component)
	end
end

function sys_resume_card.deactivate(arg_3_0)
	if arg_3_0.deactiveAnimation then
		arg_3_0.deactiveAnimation:run(arg_3_0)
	end

	if arg_3_0.active_component then
		arg_3_0.active_component:disable()
	end

	if arg_3_0.inactive_component then
		arg_3_0.inactive_component:enable()
	end

	arg_3_0:hideWarning()
end

function sys_resume_card.showWarning(arg_4_0, arg_4_1)
	if not arg_4_0.popup_warning then
		return
	end

	arg_4_0.popup_warning:activate(arg_4_1)
end

function sys_resume_card.hideWarning(arg_5_0)
	if not arg_5_0.popup_warning then
		return
	end

	arg_5_0.popup_warning:deactivate()
end

function sys_resume_card.update(arg_6_0, arg_6_1)
	GUIButton.update(arg_6_0, arg_6_1)

	if arg_6_0.textureIsLoading and not arg_6_0.textureResource:isLoading() then
		arg_6_0:updateTexture(true)
	end
end

function sys_resume_card.updateTexture(arg_7_0, arg_7_1)
	if arg_7_0.textureResource:isLoaded() or arg_7_0.textureResource:hasLoadError() then
		if arg_7_0.textureResource:isLoaded() then
			arg_7_0.screen:setTexture(arg_7_0.textureResource)
		elseif arg_7_0.textureResource:hasLoadError() then
			arg_7_0.screen:setTexture(arg_7_0.dummytexture)
		end

		arg_7_0.textureIsLoading = false

		tween_stop(arg_7_0.fade_tween)

		if arg_7_1 then
			arg_7_0.fade_tween = Tween:colorTo(arg_7_0.screen, 0.125, 1, 1, 1, 1):start()
		else
			arg_7_0.screen:setColor(1, 1, 1, 1)
		end
	end
end

function sys_resume_card.setInfo(arg_8_0, arg_8_1)
	if arg_8_1 == nil then
		arg_8_0.resumeinfo = nil
		arg_8_0.is_locked = false

		if arg_8_0.saved_node then
			arg_8_0.saved_node:disable()
		end

		if arg_8_0.playtimeDot then
			arg_8_0.playtimeDot:setNumber(0)
		end

		arg_8_0.textureResource = nil
		arg_8_0.textureIsLoading = false

		arg_8_0.screen:setTexture(nil)
		arg_8_0.screen:disable()

		if REED_DEBUG and arg_8_0.debug_filelabel and debug_store.enableDebugLabelPrint then
			arg_8_0.debug_filelabel:setText("EMPTY")
		end

		arg_8_0:setState("disabled")
	else
		arg_8_0.resumeinfo = arg_8_1

		if arg_8_0.saved_node then
			arg_8_0.saved_node:enable()
		end

		if arg_8_0.playtimeDot then
			arg_8_0.playtimeDot:setNumber(arg_8_1.playtime)
		end

		local var_8_0 = arg_8_1.thumbnail_resource
		local var_8_1 = not var_8_0:isLoaded() and not var_8_0:isLoading()

		if var_8_0:hasLoadError() or var_8_1 then
			var_8_0 = arg_8_0.dummytexture

			arg_8_0.screen:setTexture(var_8_0)

			arg_8_0.textureIsLoading = false
		elseif var_8_0:isLoaded() then
			arg_8_0.screen:setTexture(var_8_0)

			arg_8_0.textureIsLoading = false
		else
			arg_8_0.screen:setTexture(nil)

			arg_8_0.textureIsLoading = true
		end

		tween_stop(arg_8_0.fade_tween)

		if arg_8_0.textureIsLoading then
			arg_8_0.screen:setColor(0, 0, 0, 1)
		else
			arg_8_0.screen:setColor(1, 1, 1, 1)
		end

		arg_8_0.textureResource = var_8_0

		arg_8_0.screen:enable()
		arg_8_0:lockRefresh()

		if REED_DEBUG and arg_8_0.debug_filelabel and debug_store.enableDebugLabelPrint then
			local var_8_2, var_8_3 = arg_8_1.getStateFilename():gsub("/", " ")

			arg_8_0.debug_filelabel:setText(var_8_2)
		end

		arg_8_0:sramIconRefresh()
		arg_8_0:setState("idle")
	end
end

function sys_resume_card.hideOperation(arg_9_0)
	if arg_9_0.operation_node then
		arg_9_0.operation_node:disable()
	end
end

function sys_resume_card.showOperation(arg_10_0)
	if arg_10_0.operation_node then
		arg_10_0.operation_node:enable()
	end
end

function sys_resume_card.changeMode(arg_11_0)
	if arg_11_0.floating_component then
		arg_11_0.floating_component:enable()
	end
end

function sys_resume_card.changeModeEnd(arg_12_0)
	if arg_12_0.floating_component then
		arg_12_0.floating_component:disable()
	end
end

function sys_resume_card.showRegisterFrame(arg_13_0)
	if arg_13_0.register_frame then
		arg_13_0.register_frame:enable()
	end
end

function sys_resume_card.hideRegisterFrame(arg_14_0)
	if arg_14_0.register_frame then
		arg_14_0.register_frame:disable()
	end
end

function sys_resume_card.isEmpty(arg_15_0)
	if arg_15_0.resumeinfo then
		return false
	else
		return true
	end
end

function sys_resume_card.lockToggle(arg_16_0)
	local var_16_0 = arg_16_0.resumeinfo.game_code
	local var_16_1 = arg_16_0.resumeinfo.index

	if arg_16_0:isLocked() then
		system.set_locked_resumedata(var_16_0, var_16_1, false)

		if arg_16_0.unlockedSound then
			arg_16_0.unlockedSound:stop()
			arg_16_0.unlockedSound:play()
		end

		if arg_16_0.lock_icon_node then
			arg_16_0.lock_icon:enable()
			arg_16_0.lock_icon_node:setLocalScale(2, 2)
			tween_stop(arg_16_0.locktween)

			arg_16_0.locktween = Tween:sequence(Tween:wait(0.1), Tween:componentEnabledTo(arg_16_0.unlock_icon, 0, true), Tween:componentEnabledTo(arg_16_0.lock_icon, 0, false), Tween:wait(0.1), Tween:scaleTo(arg_16_0.lock_icon_node, 0.05, 1, 1), Tween:componentEnabledTo(arg_16_0.unlock_icon, 0, false)):start()
		end
	else
		system.set_locked_resumedata(var_16_0, var_16_1, true)

		if arg_16_0.lockedSound then
			arg_16_0.lockedSound:stop()
			arg_16_0.lockedSound:play()
		end

		if arg_16_0.lock_icon_node then
			arg_16_0.unlock_icon:enable()
			arg_16_0.lock_icon_node:setLocalScale(2, 2)
			tween_stop(arg_16_0.locktween)

			arg_16_0.locktween = Tween:sequence(Tween:wait(0.1), Tween:componentEnabledTo(arg_16_0.lock_icon, 0, true), Tween:componentEnabledTo(arg_16_0.unlock_icon, 0, false), Tween:wait(0.1), Tween:scaleTo(arg_16_0.lock_icon_node, 0.05, 1, 1)):start()
		end
	end
end

function sys_resume_card.isLocked(arg_17_0)
	if arg_17_0.resumeinfo then
		local var_17_0 = arg_17_0.resumeinfo.game_code
		local var_17_1 = arg_17_0.resumeinfo.index

		return system.is_locked_resumedata(var_17_0, var_17_1)
	end

	return false
end

function sys_resume_card.lockRefresh(arg_18_0)
	tween_stop(arg_18_0.locktween)

	if arg_18_0:isLocked() then
		if arg_18_0.lock_icon then
			arg_18_0.lock_icon:enable()
		end

		if arg_18_0.unlock_icon then
			arg_18_0.unlock_icon:disable()
		end
	else
		if arg_18_0.lock_icon then
			arg_18_0.lock_icon:disable()
		end

		if arg_18_0.unlock_icon then
			arg_18_0.unlock_icon:disable()
		end
	end

	if arg_18_0.lock_icon_node then
		arg_18_0.lock_icon_node:setLocalScale(1, 1)
	end

	if arg_18_0.unlock_icon then
		arg_18_0.unlock_icon:setAlpha(1)
	end
end

function sys_resume_card.showLockIcon(arg_19_0)
	if arg_19_0:isLocked() then
		tween_stop(arg_19_0.locktween)

		arg_19_0.locktween = Tween:parallel(Tween:sequence(Tween:wait(0.1), Tween:callback(function()
			arg_19_0.lock_icon_node:setLocalScale(2, 2)
		end), Tween:moveTo(arg_19_0.lock_icon_node, 0.1, 0, 6), Tween:moveTo(arg_19_0.lock_icon_node, 0.1, 0, -6), Tween:moveTo(arg_19_0.lock_icon_node, 0.1, 0, 6), Tween:moveTo(arg_19_0.lock_icon_node, 0.1, 0, 0), Tween:scaleTo(arg_19_0.lock_icon_node, 0.05, 1, 1)), Tween:sequence(Tween:moveTo(arg_19_0.saved_card, 0.05, CloverConst.Suspension.OVERWRITE_LOCKED_WIDTH_FIRST, 0), Tween:moveTo(arg_19_0.saved_card, 0.05, -CloverConst.Suspension.OVERWRITE_LOCKED_WIDTH_FIRST, 0), Tween:moveTo(arg_19_0.saved_card, 0.05, CloverConst.Suspension.OVERWRITE_LOCKED_WIDTH_SECOND, 0), Tween:moveTo(arg_19_0.saved_card, 0.05, 0, 0))):start()
	end
end

function sys_resume_card.lockAnimation(arg_21_0, arg_21_1)
	if arg_21_0.resumeinfo then
		tween_stop(arg_21_0.locktween)

		if arg_21_0:isLocked() then
			if arg_21_0.lock_icon then
				arg_21_0.lock_icon:enable()
			end

			if arg_21_0.unlock_icon then
				arg_21_0.unlock_icon:disable()
			end
		else
			if arg_21_0.lock_icon then
				arg_21_0.lock_icon:disable()
			end

			if arg_21_0.unlock_icon then
				arg_21_0.unlock_icon:enable()
			end
		end

		if arg_21_1 == 0 then
			arg_21_0.lock_icon_node:setLocalScale(2, 2)
		else
			arg_21_0.lock_icon_node:setLocalScale(1, 1)

			arg_21_0.locktween = Tween:scaleTo(arg_21_0.lock_icon_node, arg_21_1, 2, 2):start()
		end
	end
end

function sys_resume_card.sramIconRefresh(arg_22_0)
	if not arg_22_0.resumeinfo then
		return
	end

	if arg_22_0.sram_icon then
		arg_22_0.is_need_warning = false

		if arg_22_0:isSupportedSave() then
			local var_22_0 = arg_22_0.resumeinfo
			local var_22_1 = CloverTitleAccesor.new(var_22_0.game_code)

			if arg_22_0.is_floating == false then
				local var_22_2 = var_22_1:getCartridgeSramHash()
				local var_22_3 = var_22_1:getSuspendPointSramHash(var_22_0.index)

				if FileUtils.fileExists(var_22_2) then
					if not FileUtils.fileExists(var_22_3) then
						arg_22_0.is_need_warning = true
					elseif not system.compareFilesContent(var_22_2, var_22_3) then
						arg_22_0.is_need_warning = true
					end
				end
			end
		end

		local var_22_4 = "none"

		arg_22_0.sram_icon:setMode(var_22_4)
	end
end

function sys_resume_card.isNeedWarning(arg_23_0)
	return arg_23_0.is_need_warning
end

function sys_resume_card.expandAnimation(arg_24_0)
	arg_24_0:setZIndex(20)

	local var_24_0 = 0.1
	local var_24_1 = {}

	local function var_24_2(arg_25_0)
		for iter_25_0 in iterate_children(arg_25_0) do
			local var_25_0 = iter_25_0:getComponents(VisualComponent)

			for iter_25_1, iter_25_2 in ipairs(var_25_0) do
				local var_25_1 = Tween:alphaTo(iter_25_2, var_24_0, 0)

				table.insert(var_24_1, var_25_1)
			end

			var_24_2(iter_25_0)
		end
	end

	var_24_2(arg_24_0.saved_card)
	Tween:wait(system.delayFadeOut or 0):parallel(Tween:scaleTo(arg_24_0.saved_card, var_24_0, 2, 2), unpack(var_24_1)):wait(0.4):callback(function()
		if HOST_PLATFORM_IS_WINDOWS then
			arg_24_0:expandAnimation_reset()
		end
	end):start()
end

function sys_resume_card.expandAnimation_reset(arg_27_0)
	local function var_27_0(arg_28_0)
		for iter_28_0 in iterate_children(arg_28_0) do
			local var_28_0 = iter_28_0:getComponents(VisualComponent)

			for iter_28_1, iter_28_2 in ipairs(var_28_0) do
				iter_28_2:setAlpha(1)
			end

			var_27_0(iter_28_0)
		end
	end

	var_27_0(arg_27_0.saved_card)
	arg_27_0.saved_card:setLocalScale(1, 1)
	arg_27_0:setZIndex(0)
end

function sys_resume_card.is_floating_card(arg_29_0)
	if not arg_29_0.resumeinfo then
		return false
	end

	if arg_29_0.resumeinfo.index == 0 then
		return true
	end

	return false
end

function sys_resume_card.isSupportedSave(arg_30_0)
	if not arg_30_0.resumeinfo then
		return false
	end

	return titles_list[arg_30_0.resumeinfo.game_code].save_count ~= 0
end
