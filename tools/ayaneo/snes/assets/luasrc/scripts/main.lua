require("/scripts/core/core.lua")
require("/scripts/system.lua")
require("/scripts/Store_myplay_data.lua")
require("/scripts/app/clover_util.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_demo.lua")
require("/scripts/app/clover_bg.lua")
require("/scripts/app/clover_scene.lua")
require("/scripts/app/clover_transition.lua")
require("/scripts/app/clover_autocancel.lua")
require("/scripts/app/clover_elements.lua")

Main = class(gui_container)

local var_0_0
local var_0_1

function Main.start(arg_1_0)
	gui_container.start(arg_1_0)
	CloverSingleton.Bind(Main, arg_1_0)
	CloverScene.Init(arg_1_0)
	CloverTransition:Init(arg_1_0)
	CloverAutoCancel.Init(arg_1_0)

	local var_1_0 = arg_1_0.elementArray or getObjects(arg_1_0, gui_container)
	local var_1_1 = {}

	for iter_1_0, iter_1_1 in pairs(var_1_0) do
		var_1_1[iter_1_1:getName()] = iter_1_1
	end

	arg_1_0.scenesList = var_1_0
	arg_1_0.scenesTable = var_1_1
	arg_1_0.leaveGUIForcusCount = 0
	arg_1_0.current = arg_1_0.startScene

	arg_1_0.current:enable()
	arg_1_0.current:activate()

	if not store.language_selected and system.is_nes() or debug_store.force_firstLanguageSelect then
		if REED_DEBUG then
			Store:delete_uisave()
		end

		arg_1_0:setCurrentScene("option_languages_first")
		arg_1_0.scenesTable.option_languages_first:enable()
		CloverDemo.Disable()
	else
		arg_1_0.scenesTable.option_languages_first:disable()
	end

	arg_1_0.bgmPhase = 0

	arg_1_0.resMainBGM:link()
	CloverDemo.Init(arg_1_0.demoScene)
	MyplayData.Init()
	CloverBG.Init(arg_1_0.bgScene, sys_boot:getInstance().camera)

	var_0_0 = arg_1_0.debugLabel

	DbgMenu:entry("Main", arg_1_0)

	if not debug_store.debugBGMOff then
		local var_1_2 = arg_1_0:getComponent(SoundComponent)

		if var_1_2 then
			var_1_2:play()
		else
			print("Error: Main:start: not found BGM SoundComponent")
		end
	else
		print("Main:start: BGM OFF")
	end

	if system.is_roll_back_cancel then
		arg_1_0.scenesTable.resumemenu:setFirstActivate(true)
		arg_1_0:toResumeMenu()
		arg_1_0.scenesTable.resumemenu:setFirstActivate(false)
	end

	CloverElements.gametitlelist:setUnlockEvent(arg_1_0.unlock_event)
end

function Main.cbLanguage(arg_2_0, arg_2_1)
	print(string.format("CAPTION=%s VALUE=%s", arg_2_1.caption, arg_2_1:get().caption))
	system.setLocale(arg_2_1:get().language, "", arg_2_1:get().region)
end

function Main.cbTest(arg_3_0, arg_3_1)
	print(string.format("CAPTION=%s VALUE=%d", arg_3_1.caption, arg_3_1:get()))
end

function Main.update(arg_4_0, arg_4_1)
	gui_container.update(arg_4_0, arg_4_1)

	if arg_4_0.bgmPhase then
		if arg_4_0.bgmPhase == 0 then
			if arg_4_0.resMainBGM:isLoaded() then
				arg_4_0.bgmPhase = 1

				arg_4_0.mainBGM:setSoundResource(arg_4_0.resMainBGM)
			end
		elseif arg_4_0.bgmPhase == 1 then
			local var_4_0 = false
			local var_4_1 = arg_4_0.bootBGM:getSamplePosition()

			if arg_4_0.bgmSamplePosition and var_4_1 < arg_4_0.bgmSamplePosition then
				var_4_0 = true
			else
				arg_4_0.bgmSamplePosition = var_4_1
			end

			if not debug_store.debugBGMOff and (var_4_0 or not arg_4_0.bootBGM:isPlaying()) then
				arg_4_0.mainBGM:play()
				arg_4_0.bootBGM:disable()

				arg_4_0.bgmPhase = nil
			end
		end
	end

	local var_4_2 = arg_4_0.dbg_menu and arg_4_0.dbg_menu:isEnabled()

	if GUI.focusedElement == nil and not var_4_2 and CloverPadUI.down:isAnything() then
		print("GUI.focusedElement is nil!!")
	end

	if CLOVER_IS_DEBUG then
		if CloverPadDbg.pressed.btn_l then
			if not arg_4_0.dbg_menu:isEnabled() then
				arg_4_0:leaveGUIForcus()
				gui_custom:pushGUIStatus("DbgMenu")
				arg_4_0.dbg_menu:enable()
				arg_4_0.dbg_menu:open()
				CloverDemo.Disable()
			else
				arg_4_0:unleaveGUIForcus()
				gui_custom:popGUIStatus()
				arg_4_0.dbg_menu:disable()
				arg_4_0.dbg_menu:close()
				CloverDemo.Enable()
			end
		end

		if var_0_1 then
			for iter_4_0, iter_4_1 in pairs(var_0_1) do
				if iter_4_1.updateText then
					iter_4_1:setText(iter_4_1:updateText())
				end
			end
		end
	end
end

function Main.leaveGUIForcus(arg_5_0)
	if arg_5_0.leaveGUIForcusCount == 0 then
		arg_5_0.forcusedElement = GUI.focusedElement

		GUI:focusElement(nil)
		system.cursor:cursorHide()
	end

	arg_5_0.leaveGUIForcusCount = arg_5_0.leaveGUIForcusCount + 1
end

function Main.unleaveGUIForcus(arg_6_0)
	if arg_6_0.leaveGUIForcusCount > 0 then
		arg_6_0.leaveGUIForcusCount = arg_6_0.leaveGUIForcusCount - 1

		if arg_6_0.leaveGUIForcusCount == 0 then
			GUI:focusElement(arg_6_0.forcusedElement, true)

			arg_6_0.forcusedElement = nil
		end
	end
end

function Main.toHomeMenu(arg_7_0)
	if arg_7_0 == Main then
		arg_7_0 = Main.instance
	end

	CloverDemo.Enable()
	arg_7_0:setCurrentScene("homemenu")
end

function Main.toResumeMenu(arg_8_0)
	if arg_8_0 == Main then
		arg_8_0 = Main.instance
	end

	CloverDemo.Disable()
	arg_8_0:setCurrentScene("resumemenu")
end

function Main.toDisplay(arg_9_0)
	if arg_9_0 == Main then
		arg_9_0 = Main.instance
	end

	CloverDemo.Disable()
	arg_9_0:setCurrentScene("option_display")
end

function Main.toSettings(arg_10_0)
	if arg_10_0 == Main then
		arg_10_0 = Main.instance
	end

	CloverDemo.Disable()
	arg_10_0:setCurrentScene("option_settings")
end

function Main.toLanguages(arg_11_0)
	if arg_11_0 == Main then
		arg_11_0 = Main.instance
	end

	CloverDemo.Disable()
	arg_11_0:setCurrentScene("option_languages")
end

function Main.toCopyRight(arg_12_0)
	if arg_12_0 == Main then
		arg_12_0 = Main.instance
	end

	CloverDemo.Disable()
	arg_12_0:setCurrentScene("copyright")
end

function Main.toManual(arg_13_0)
	if arg_13_0 == Main then
		arg_13_0 = Main.instance
	end

	CloverDemo.Disable()
	arg_13_0:setCurrentScene("manual")
end

function Main.setCurrentScene(arg_14_0, arg_14_1)
	print(arg_14_1)
	arg_14_0.current:deactivate()

	if arg_14_0.scene_change_cursor_z then
		system.cursor:setNextStartZ(arg_14_0.scene_change_cursor_z)

		arg_14_0.scene_change_cursor_z = nil
	end

	arg_14_0.current = arg_14_0.scenesTable[arg_14_1]

	GUI:focusElement(arg_14_0.current)
	arg_14_0.current:activate()
	system.cursor:setNextStartZ(0)
end

function Main.setSceneChangeCursorZ(arg_15_0, arg_15_1)
	if arg_15_0 == Main then
		arg_15_0 = Main.instance
	end

	arg_15_0.scene_change_cursor_z = arg_15_1

	CloverElements.gametitlelist:setNextCursorZ(nil)
end

function Main.stopMainBGM(arg_16_0)
	if arg_16_0 == Main then
		arg_16_0 = Main.instance
	end

	if arg_16_0.resMainBGM:isLoaded() then
		arg_16_0.mainBGM:stop()
	end
end

function Main.playMainBGM(arg_17_0)
	if arg_17_0 == Main then
		arg_17_0 = Main.instance
	end

	if arg_17_0.resMainBGM:isLoaded() then
		arg_17_0.mainBGM:play()
	end
end

function Main.setVolumeMainBGM(arg_18_0, arg_18_1)
	if arg_18_0 == Main then
		arg_18_0 = Main.instance
	end

	if arg_18_0.resMainBGM:isLoaded() then
		arg_18_0.mainBGM:setVolume(arg_18_1)
	end
end

function Main.resetVolumeMainBGM(arg_19_0)
	arg_19_0:setVolumeMainBGM(1)
end

if REED_DEBUG then
	function debugLabelPrint(arg_20_0)
		if var_0_0 then
			if store and not debug_store.enableDebugLabelPrint then
				var_0_0:disable()

				return
			else
				var_0_0:enable()
			end

			var_0_0:setText(arg_20_0)
			var_0_0:setColor(1, 0.2, 0.2, 1)
			tween_stop(var_0_0.tween)

			var_0_0.tween = Tween:colorTo(var_0_0, 0.2, 1, 1, 1, 1):start()

			print("debugLabelPrint : \"" .. arg_20_0 .. "\"")
		end
	end

	function debugLabelDraw(arg_21_0, arg_21_1, arg_21_2, arg_21_3, arg_21_4)
		if not var_0_0 then
			return
		end

		if not var_0_1 then
			var_0_1 = {}
		end

		local var_21_0 = var_0_1[arg_21_0]

		if arg_21_1 then
			if not var_21_0 then
				var_21_0 = createComponentFromType(COMPONENT_TYPE_LABEL)

				var_21_0:setFontResource(var_0_0:getFontResource())

				var_0_1[arg_21_0] = var_21_0

				var_0_0:getNode():getParentNode():attachComponent(var_21_0)
			end

			var_21_0:setTextOffset(arg_21_2, arg_21_3)

			var_21_0.updateText = arg_21_1

			var_21_0:setZIndex(arg_21_4)
			var_21_0:enable()

			return var_21_0
		elseif var_21_0 then
			var_21_0:disable()

			var_21_0.updateText = nil
		end

		return nil
	end
else
	function debugLabelPrint(arg_22_0)
		print("debugLabelPrint : \"" .. tostring(arg_22_0) .. "\"")
	end

	function debugDrawLabel(arg_23_0, arg_23_1, arg_23_2, arg_23_3)
		return
	end
end
