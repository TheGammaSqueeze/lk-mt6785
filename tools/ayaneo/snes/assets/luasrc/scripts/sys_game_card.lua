require("/scripts/core/core.lua")
require("/scripts/helper_tween.lua")

sys_game_card = class(GUIButton)

-- all cards render at full size; the selected one is marked by its blue frame and
-- cyan cursor glow, not by scale (overlay-matching the reference showed no size
-- difference between the selected card and its neighbors)
local INACTIVE_CARD_SCALE = 1.0

local var_0_0 = {
	"card",
	"player1_icon",
	"player2_icon",
	"player2s_icon",
	"resume_icon1e",
	"resume_icon2e",
	"resume_icon3e",
	"resume_icon4e",
	"resume_icon1d",
	"resume_icon2d",
	"resume_icon3d",
	"resume_icon4d",
	"resume_icon1l",
	"resume_icon2l",
	"resume_icon3l",
	"resume_icon4l"
}

function sys_game_card.start(arg_1_0)
	GUIButton.start(arg_1_0)

	if arg_1_0.screen then
		arg_1_0.screenX, arg_1_0.screenY = arg_1_0.screen:getSize()
	end

	arg_1_0.inactive_node:enable()
	arg_1_0.active_node:disable()
	arg_1_0:setLocalScale(INACTIVE_CARD_SCALE, INACTIVE_CARD_SCALE)

	for iter_1_0, iter_1_1 in pairs(var_0_0) do
		arg_1_0.active_node[iter_1_1]:setAlpha(0)
	end

	if not REED_DEBUG and arg_1_0.debug_sortlabel then
		arg_1_0.debug_sortlabel:disable()
	end
end

function sys_game_card.activate(arg_2_0)
	if arg_2_0.activeAnimation then
		arg_2_0.activeAnimation:run(arg_2_0)
	end
end

function sys_game_card.deactivate(arg_3_0)
	if arg_3_0.deactiveAnimation then
		arg_3_0.deactiveAnimation:run(arg_3_0)
	end
end

local var_0_1 = false

function sys_game_card.enableActiveComponent(arg_4_0, arg_4_1)
	-- Only skip re-enabling when the card is genuinely fully active: active node on, inactive
	-- off, AND the box art actually visible. Under fast scroll an interrupted alpha fade can
	-- leave active enabled + inactive disabled but the card/icons at alpha 0 (blank selected
	-- card); without the alpha check this guard would short-circuit and never heal it.
	if arg_4_0.active_node:isEnabled() and not arg_4_0.inactive_node:isEnabled() and arg_4_0.active_node.card:getAlpha() >= 1 then
		return
	end

	arg_4_1 = arg_4_1 or 0

	tween_stop(arg_4_0.scaleTween)
	if arg_4_1 and arg_4_1 > 0 then
		arg_4_0.scaleTween = Tween:scaleTo(arg_4_0, arg_4_1, 1, 1):start()
	else
		arg_4_0:setLocalScale(1, 1)
	end

	if var_0_1 or arg_4_1 == 0 then
		arg_4_0.inactive_node:disable()
		arg_4_0.active_node:enable()
		tween_stop(arg_4_0.activeTween)

		for iter_4_0, iter_4_1 in pairs(var_0_0) do
			arg_4_0.active_node[iter_4_1]:setAlpha(1)
		end
	else
		arg_4_0.inactive_node:enable()
		arg_4_0.active_node:enable()
		tween_stop(arg_4_0.activeTween)

		local var_4_0 = {}

		for iter_4_2, iter_4_3 in pairs(var_0_0) do
			var_4_0[#var_4_0 + 1] = Tween:alphaTo(arg_4_0.active_node[iter_4_3], arg_4_1, 1):callback(function()
				return
			end)
		end

		arg_4_0.activeTween = Tween:sequence(Tween:parallel(unpack(var_4_0)):callback(function()
			arg_4_0.inactive_node:disable()
		end)):start()
	end
end

function sys_game_card.disableActiveComponent(arg_7_0, arg_7_1)
	if not arg_7_0.active_node:isEnabled() and arg_7_0.inactive_node:isEnabled() then
		return
	end

	arg_7_1 = arg_7_1 or 0

	tween_stop(arg_7_0.scaleTween)
	if arg_7_1 and arg_7_1 > 0 then
		arg_7_0.scaleTween = Tween:scaleTo(arg_7_0, arg_7_1, INACTIVE_CARD_SCALE, INACTIVE_CARD_SCALE):start()
	else
		arg_7_0:setLocalScale(INACTIVE_CARD_SCALE, INACTIVE_CARD_SCALE)
	end

	if var_0_1 or arg_7_1 == 0 then
		arg_7_0.inactive_node:enable()
		arg_7_0.active_node:disable()
		tween_stop(arg_7_0.activeTween)

		for iter_7_0, iter_7_1 in pairs(var_0_0) do
			arg_7_0.active_node[iter_7_1]:setAlpha(0)
		end
	else
		arg_7_0.inactive_node:enable()
		arg_7_0.active_node:enable()
		tween_stop(arg_7_0.activeTween)

		local var_7_0 = {}

		for iter_7_2, iter_7_3 in pairs(var_0_0) do
			var_7_0[#var_7_0 + 1] = Tween:alphaTo(arg_7_0.active_node[iter_7_3], arg_7_1, 0):callback(function()
				return
			end)
		end

		arg_7_0.activeTween = Tween:sequence(Tween:parallel(unpack(var_7_0)):callback(function()
			arg_7_0.active_node:disable()
		end)):start()
	end
end

function sys_game_card.update(arg_10_0, arg_10_1)
	local function var_10_0()
		return arg_10_0.screen and arg_10_0.gameinfo.spriteResource and arg_10_0.gameinfo.spriteResource:isLoaded()
	end

	if arg_10_0.loading and var_10_0() then
		arg_10_0:loadingDone()

		arg_10_0.loading = nil
		system.loadingCount = system.loadingCount - 1
	end

	if arg_10_0.is_copy and var_10_0() then
		arg_10_0:loadingDone()

		arg_10_0.is_copy = nil
	end

	GUIButton.update(arg_10_0, arg_10_1)
end

function sys_game_card.setInfo(arg_12_0, arg_12_1, arg_12_2)
	arg_12_0.gameinfo = arg_12_1

	local var_12_0 = arg_12_1.simultaneous == 1

	arg_12_0.active_node:setPlayers(arg_12_1.players, var_12_0)
	arg_12_0.inactive_node:setPlayers(arg_12_1.players, var_12_0)

	arg_12_0.is_copy = arg_12_2

	if not arg_12_0.is_copy then
		if arg_12_0.gameinfo.spriteResource:isLoading() then
			arg_12_0.loading = true
			system.loadingCount = system.loadingCount + 1
		elseif arg_12_0.gameinfo.spriteResource:isLoaded() then
			arg_12_0:loadingDone()
		end
	end

	arg_12_0:refreshInfo()

	local var_12_1 = "none"

	if arg_12_0.gameinfo.save_count ~= 0 then
		var_12_1 = "on"
	end

	arg_12_0.sram_icon:setMode(var_12_1)
end

function sys_game_card.refreshInfo(arg_13_0)
	local var_13_0 = arg_13_0.gameinfo

	arg_13_0.active_node:setResumeInfo(var_13_0.game_code)
	arg_13_0.inactive_node:setResumeInfo(var_13_0.game_code)

	if arg_13_0.sub_cursor then
		if system.floating_save and system.floating_save.game_code == var_13_0.game_code then
			arg_13_0.sub_cursor:enable()
		else
			arg_13_0.sub_cursor:disable()
		end
	end
end

function sys_game_card.loadingDone(arg_14_0)
	local var_14_0, var_14_1 = arg_14_0.gameinfo.spriteResource:getSize()
	local var_14_2 = arg_14_0.screenX
	local var_14_3 = arg_14_0.screenX

	arg_14_0.screen:setTexture(arg_14_0.gameinfo.spriteResource)

	local var_14_4 = var_14_2 / var_14_0
	local var_14_5 = var_14_3 / var_14_1
	local var_14_6 = math.min(var_14_4, var_14_5, 1)
	local var_14_7 = var_14_0 * var_14_6
	local var_14_8 = var_14_1 * var_14_6

	arg_14_0.screen:setSize(var_14_7, var_14_8)
end

function sys_game_card.setCursor(arg_15_0)
	if system.cursor then
		system.cursor:setRounded(arg_15_0.cursor_component, true)
	end
end

function sys_game_card.setDebugLabel(arg_16_0, arg_16_1)
	if REED_DEBUG then
		arg_16_0.debug_sortlabel:setText(arg_16_1)
	end
end

function sys_game_card.showWarning(arg_17_0)
	if not arg_17_0.backup_warning then
		return
	end

	if system.isPlayingDemo() then
		return
	end

	if arg_17_0.gameinfo.save_count == 0 then
		return
	end

	if arg_17_0.backup_warning:isEnabled() then
		return
	end

	arg_17_0.backup_warning:setWarningType("copy")
	arg_17_0.backup_warning:activate()
end

function sys_game_card.hideWarning(arg_18_0)
	if not arg_18_0.backup_warning then
		return
	end

	arg_18_0.backup_warning:deactivate()
end

function sys_game_card.expandAnimation(arg_19_0)
	arg_19_0:setCursor()
	arg_19_0:setZIndex(20)

	local var_19_0 = 0.1
	local var_19_1 = {}

	local function var_19_2(arg_20_0)
		for iter_20_0 in iterate_children(arg_20_0) do
			local var_20_0 = iter_20_0:getComponents(VisualComponent)

			for iter_20_1, iter_20_2 in ipairs(var_20_0) do
				local var_20_1 = Tween:alphaTo(iter_20_2, var_19_0, 0)

				table.insert(var_19_1, var_20_1)
			end

			var_19_2(iter_20_0)
		end
	end

	var_19_2(arg_19_0)
	Tween:wait(system.delayFadeOut or 0):parallel(Tween:scaleTo(arg_19_0, var_19_0, 2, 2), unpack(var_19_1)):wait(0.4):callback(function()
		if HOST_PLATFORM_IS_WINDOWS then
			arg_19_0:expandAnimation_reset()
		end
	end):start()
end

function sys_game_card.expandAnimation_reset(arg_22_0)
	local function var_22_0(arg_23_0)
		for iter_23_0 in iterate_children(arg_23_0) do
			local var_23_0 = iter_23_0:getComponents(VisualComponent)

			for iter_23_1, iter_23_2 in ipairs(var_23_0) do
				iter_23_2:setAlpha(1)
			end

			var_22_0(iter_23_0)
		end
	end

	var_22_0(arg_22_0)
	arg_22_0:setLocalScale(1, 1)
	arg_22_0:setZIndex(0)
end

function sys_game_card.unlock_shake_cancel(arg_24_0)
	local var_24_0 = 0
	local var_24_1 = 0
	local var_24_2 = 0
	local var_24_3 = 24
	local var_24_4 = 0
	local var_24_5 = 0

	tween_stop(arg_24_0.shakeTween)
	arg_24_0:getChildByName("non_active"):setLocalPosition(var_24_0, var_24_1)
	arg_24_0:getChildByName("screen"):setLocalPosition(var_24_2, var_24_3)
	arg_24_0:getChildByName("lock"):setLocalPosition(var_24_4, var_24_5)
end

function sys_game_card.unlock_shake(arg_25_0)
	local var_25_0 = 0
	local var_25_1 = 0
	local var_25_2 = 0
	local var_25_3 = 24
	local var_25_4 = 0
	local var_25_5 = 0
	local var_25_6 = 0.05
	local var_25_7 = 0.05
	local var_25_8 = 5
	local var_25_9 = 5

	tween_stop(arg_25_0.shakeTween)

	arg_25_0.shakeTween = Tween:parallel(Tween:sequence(Tween:moveTo(arg_25_0:getChildByName("non_active"), var_25_6, var_25_0 + var_25_9, var_25_1 + var_25_8), Tween:moveTo(arg_25_0:getChildByName("non_active"), var_25_7, var_25_0 - var_25_9, var_25_1), Tween:moveTo(arg_25_0:getChildByName("non_active"), var_25_6, var_25_0 + var_25_9, var_25_1 + var_25_8 / 2), Tween:moveTo(arg_25_0:getChildByName("non_active"), var_25_7, var_25_0 - var_25_9, var_25_1), Tween:moveTo(arg_25_0:getChildByName("non_active"), var_25_6, var_25_0, var_25_1)), Tween:sequence(Tween:moveTo(arg_25_0:getChildByName("screen"), var_25_6, var_25_2 + var_25_9, var_25_3 + var_25_8), Tween:moveTo(arg_25_0:getChildByName("screen"), var_25_7, var_25_2 - var_25_9, var_25_3), Tween:moveTo(arg_25_0:getChildByName("screen"), var_25_6, var_25_2 + var_25_9, var_25_3 + var_25_8 / 2), Tween:moveTo(arg_25_0:getChildByName("screen"), var_25_7, var_25_2 - var_25_9, var_25_3), Tween:moveTo(arg_25_0:getChildByName("screen"), var_25_6, var_25_2, var_25_3)), Tween:sequence(Tween:moveTo(arg_25_0:getChildByName("lock"), var_25_6, var_25_4 + var_25_9, var_25_5 + var_25_8), Tween:moveTo(arg_25_0:getChildByName("lock"), var_25_7, var_25_4 - var_25_9, var_25_5), Tween:moveTo(arg_25_0:getChildByName("lock"), var_25_6, var_25_4 + var_25_9, var_25_5 + var_25_8 / 2), Tween:moveTo(arg_25_0:getChildByName("lock"), var_25_7, var_25_4 - var_25_9, var_25_5), Tween:moveTo(arg_25_0:getChildByName("lock"), var_25_6, var_25_4, var_25_5))):start()
end

function sys_game_card.unlock_openbox_shake(arg_26_0)
	local var_26_0 = 0
	local var_26_1 = 0
	local var_26_2 = 0
	local var_26_3 = 24
	local var_26_4 = 0
	local var_26_5 = 0
	local var_26_6 = 15
	local var_26_7 = -102
	local var_26_8 = 0.05
	local var_26_9 = 0.06
	local var_26_10 = 0.1
	local var_26_11 = 2
	local var_26_12 = 3

	tween_stop(arg_26_0.activeTween)

	arg_26_0.activeTween = Tween:sequence(Tween:parallel(Tween:loop(6, Tween:sequence(Tween:moveTo(arg_26_0:getChildByName("non_active"), var_26_8, var_26_0 + var_26_12, var_26_1 + var_26_11), Tween:moveTo(arg_26_0:getChildByName("non_active"), var_26_9, var_26_0 - var_26_12, var_26_1), Tween:moveTo(arg_26_0:getChildByName("non_active"), var_26_8, var_26_0 + var_26_12, var_26_1 + var_26_11 / 2), Tween:moveTo(arg_26_0:getChildByName("non_active"), var_26_9, var_26_0 - var_26_12, var_26_1), Tween:moveTo(arg_26_0:getChildByName("non_active"), var_26_8, var_26_0 + var_26_12, var_26_1 + var_26_11), Tween:moveTo(arg_26_0:getChildByName("non_active"), var_26_9, var_26_0 - var_26_12, var_26_1))), Tween:loop(6, Tween:sequence(Tween:moveTo(arg_26_0:getChildByName("unlock"), var_26_8, var_26_4 + var_26_12, var_26_5 + var_26_11), Tween:moveTo(arg_26_0:getChildByName("unlock"), var_26_9, var_26_4 - var_26_12, var_26_5), Tween:moveTo(arg_26_0:getChildByName("unlock"), var_26_8, var_26_4 + var_26_12, var_26_5 + var_26_11 / 2), Tween:moveTo(arg_26_0:getChildByName("unlock"), var_26_9, var_26_4 - var_26_12, var_26_5), Tween:moveTo(arg_26_0:getChildByName("unlock"), var_26_8, var_26_4 + var_26_12, var_26_5 + var_26_11), Tween:moveTo(arg_26_0:getChildByName("unlock"), var_26_9, var_26_4 - var_26_12, var_26_5))), Tween:loop(6, Tween:sequence(Tween:moveTo(arg_26_0:getChildByName("screen"), var_26_8, var_26_2 + var_26_12, var_26_3 + var_26_11), Tween:moveTo(arg_26_0:getChildByName("screen"), var_26_9, var_26_2 - var_26_12, var_26_3), Tween:moveTo(arg_26_0:getChildByName("screen"), var_26_8, var_26_2 + var_26_12, var_26_3 + var_26_11 / 2), Tween:moveTo(arg_26_0:getChildByName("screen"), var_26_9, var_26_2 - var_26_12, var_26_3), Tween:moveTo(arg_26_0:getChildByName("screen"), var_26_8, var_26_2 + var_26_12, var_26_3 + var_26_11), Tween:moveTo(arg_26_0:getChildByName("screen"), var_26_9, var_26_2 - var_26_12, var_26_3)))), Tween:parallel(Tween:moveTo(arg_26_0:getChildByName("non_active"), var_26_10, var_26_0, var_26_1), Tween:moveTo(arg_26_0:getChildByName("unlock"), var_26_10, var_26_4, var_26_5), Tween:moveTo(arg_26_0:getChildByName("screen"), var_26_10, var_26_2, var_26_3)), Tween:wait(0.1):callback(function()
		if arg_26_0.unlock_open_effect then
			arg_26_0.unlock_open_effect:stop()
			arg_26_0.unlock_open_effect:play()
		end
	end), Tween:wait(0.75):callback(function()
		arg_26_0.unlock_dialog_message:enable()
	end)):start()
end

function sys_game_card.unlock_openbox_start(arg_29_0)
	arg_29_0.lock:disable()

	arg_29_0.root = arg_29_0:getChildByName("unlock")

	arg_29_0.root:enable()

	arg_29_0.anim = arg_29_0.root:getComponent(AnimatorComponent)

	arg_29_0.anim:setSceneAnimation(arg_29_0.root.unlock_start)
	arg_29_0.anim:setLoop(false)

	if arg_29_0.unlock_tremble then
		arg_29_0.unlock_tremble:stop()
		arg_29_0.unlock_tremble:play()
	end

	arg_29_0:unlock_openbox_shake()
end

function sys_game_card.unlock_openbox_loop(arg_30_0)
	arg_30_0.root = arg_30_0:getChildByName("unlock")
	arg_30_0.anim = arg_30_0.root:getComponent(AnimatorComponent)

	arg_30_0.anim:setSceneAnimation(arg_30_0.root.unlock_loop)
	arg_30_0.anim:setLoop(false)
end

function sys_game_card.unlock_openbox_end(arg_31_0)
	arg_31_0.root = arg_31_0:getChildByName("unlock")
	arg_31_0.anim = arg_31_0.root:getComponent(AnimatorComponent)

	arg_31_0.anim:setSceneAnimation(arg_31_0.root.unlock_end)
	arg_31_0.anim:setLoop(false)
end

function sys_game_card.unlock_wait_anime(arg_32_0)
	arg_32_0.root = arg_32_0:getChildByName("unlock")
	arg_32_0.anim = arg_32_0.root:getComponent(AnimatorComponent)

	if arg_32_0.anim:getCurrentTime() >= arg_32_0.anim:getDuration() then
		return true
	end

	return nil
end

function sys_game_card.cardColorToDark(arg_33_0, arg_33_1, arg_33_2, arg_33_3)
	local var_33_0 = 0.05
	local var_33_1 = 1
	local var_33_2 = 1
	local var_33_3 = 1
	local var_33_4 = 1
	local var_33_5 = 0

	if arg_33_1 then
		var_33_0 = 0.1
		var_33_5 = 0.1
		var_33_1 = var_33_1 * 0.5
		var_33_2 = var_33_2 * 0.5
		var_33_3 = var_33_3 * 0.5
	end

	if arg_33_2 then
		arg_33_0.inactive_node.card:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
		arg_33_0.screen:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
		arg_33_0.sub_cursor:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
		arg_33_0.lock:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
		arg_33_0.sram_icon:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
		arg_33_0.icon1P:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
		arg_33_0.icon2P:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
		arg_33_0.icon1P2P:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
	else
		tween_stop(arg_33_0.colorTween)

		local var_33_6 = {}

		var_33_6[#var_33_6 + 1] = Tween:colorTo(arg_33_0.inactive_node.card, var_33_0, var_33_1, var_33_2, var_33_3, 1)
		var_33_6[#var_33_6 + 1] = Tween:colorTo(arg_33_0.screen, var_33_0, var_33_1, var_33_2, var_33_3, 1)
		var_33_6[#var_33_6 + 1] = Tween:colorTo(arg_33_0.sub_cursor, var_33_0, var_33_1, var_33_2, var_33_3, 1)
		var_33_6[#var_33_6 + 1] = Tween:wait(var_33_5):callback(function()
			arg_33_0.sram_icon:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
			arg_33_0.icon1P:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
			arg_33_0.icon2P:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
			arg_33_0.icon1P2P:setColor(var_33_1, var_33_2, var_33_3, var_33_4)
		end)

		if not arg_33_3 then
			var_33_6[#var_33_6 + 1] = Tween:colorTo(arg_33_0.lock, var_33_0, var_33_1, var_33_2, var_33_3, 1)
		end

		arg_33_0.colorTween = Tween:sequence(Tween:wait(var_33_5), Tween:parallel(unpack(var_33_6)):callback(function()
			return
		end)):start()
	end
end
