require("/scripts/core/core.lua")
require("/scripts/system.lua")
require("/scripts/gui/gui_inputs.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_dialog.lua")
require("/scripts/app/clover_task.lua")

sys_boot = class(WorldNode)
bootTime = 0

local var_0_0 = 1
local var_0_1 = 0
local var_0_2 = 10

if not HOST_PLATFORM_IS_WINDOWS then
	GUI.gameDevice = GAME_DEVICE_1
else
	GUI.gameDevice = GAME_DEVICE
end

function sys_boot.exitFunction(arg_1_0)
	system.exitEvent = true

	system.fadeOut(Application.exit)
end

function sys_boot.start(arg_2_0)
	print("TITLE_RETURN_CODE = " .. tostring(TITLE_RETURN_CODE))
	arg_2_0.camera:disable()
	Pad.Init()
	CloverPadUI:EnableUserInput(false, "boot")
	CloverTask.Init()

	sys_boot.instance = arg_2_0

	if arg_2_0.bootSound then
		arg_2_0.bootSound:play()
	end

	system.load_setting()

	var_0_1 = 1

	DbgMenu:entry("sys_boot", arg_2_0)

	GUI.disabled = true
	arg_2_0.fonts = {}

	Application.setExitHandler(sys_boot.exitFunction, arg_2_0)

	arg_2_0.dialogclosed = true
end

local var_0_3 = 5
local var_0_4 = 8
local var_0_5 = {
	function(arg_3_0)
		if HOST_PLATFORM_IS_LINUX and arg_3_0.dialogclosed then
			Application.skipDraw()
		end

		if not Store:isBusy() then
			if PREVIOUS_COMMAND == "" then
				store.play_count = 0
			end

			Store:checkOldData()
			tween_registerProperty()
			system.init_title()
			arg_3_0.suspention:init()
			gui_custom:initialize()
			arg_3_0.defaultScene:link()

			local var_3_0 = {}

			if system.uisave_loadfailed then
				table.insert(var_3_0, "sys_error_menuSaveLoadFail")

				system.uisave_loadfailed = nil
			end

			if system.resume_deletefailed then
				table.insert(var_3_0, "sys_error_resumeStoreFail")

				system.resume_deletefailed = nil
			end

			if TITLE_RETURN_CODE == -3 or TITLE_RETURN_CODE == -4 then
				if not system.isPlayingDemo() then
					if TITLE_RETURN_CODE == -4 then
						table.insert(var_3_0, "sys_error_replayFail")
					else
						table.insert(var_3_0, "sys_error_resumeLoadFail")
					end
				end
			elseif TITLE_RETURN_CODE and TITLE_RETURN_CODE ~= 0 and TITLE_RETURN_CODE ~= 1 then
				table.insert(var_3_0, "sys_error_gameCrash")
			end

			if not table_isEmpty(var_3_0) then
				print("-- error dialog --")

				for iter_3_0, iter_3_1 in pairs(var_3_0) do
					print(iter_3_1)
				end

				arg_3_0.dialogclosed = false

				local function var_3_1(arg_4_0)
					GUI.disabled = false

					CloverPadUI:EnableUserInput(true, "boot")
					arg_3_0.camera:enable()
					arg_3_0.dialog:setText(Localization.getText(var_3_0[arg_4_0]))
					arg_3_0.dialog:showDialog(nil, function()
						GUI.disabled = true

						Tween:wait(0.5):callback(function()
							if arg_4_0 == #var_3_0 then
								arg_3_0.dialogclosed = true
							else
								var_3_1(arg_4_0 + 1)
							end
						end):start()
					end)
				end

				var_3_1(1)
			else
				arg_3_0.dialogclosed = true
			end

			var_0_1 = var_0_1 + 1
		end
	end,
	function(arg_7_0)
		if HOST_PLATFORM_IS_LINUX and arg_7_0.dialogclosed then
			Application.skipDraw()
		end

		local var_7_0 = false
		local var_7_1 = arg_7_0.defaultScene and arg_7_0.defaultScene:isLoaded()

		if var_7_1 then
			for iter_7_0, iter_7_1 in pairs(arg_7_0.fonts) do
				if iter_7_1 and not iter_7_1:isLoaded() then
					var_7_1 = false

					break
				end
			end
		end

		if var_7_1 and arg_7_0.dialogclosed then
			arg_7_0.camera:disable()

			local var_7_2 = arg_7_0.defaultScene:instantiate()

			arg_7_0:addChildNode(var_7_2)

			arg_7_0.defaultScene = nil
			arg_7_0.defaultSceneNode = var_7_2

			CloverDialog.Init(arg_7_0.fade, arg_7_0.dialog, arg_7_0.dialog_yesno, arg_7_0.defaultSceneNode)

			var_0_1 = var_0_1 + 1
			wait_frame = 1
		end
	end,
	function(arg_8_0)
		if HOST_PLATFORM_IS_LINUX then
			Application.skipDraw()
		end

		debugLabelPrint(("loading... %d bootTime:%f"):format(system.loadingCount, bootTime))

		if (system.TextureIsLoaded() or bootTime > var_0_2) and system.isWaitBootFade() == false then
			if not system.TextureIsLoaded() then
				print("[Warning] texture load is timeout.")
				print(system.loadingCount)
				print(system.thumbnailIsloading)
			end

			mcp.waitTransition()

			if debug_store.noBlackScreen then
				arg_8_0.fade:disable()

				GUI.disabled = false

				CloverPadUI:EnableUserInput(true, "boot")
			else
				if system.is_roll_back_cancel then
					arg_8_0.fade:setType(CloverConst.Fade.ROLLBACK_FADE_TYPE)
				end

				arg_8_0.fade:In(0, nil, function()
					GUI.disabled = false

					CloverPadUI:EnableUserInput(true, "boot")
					arg_8_0.fade:setType(CloverConst.Fade.NORMAL_FADE_TYPE)
				end)
			end

			var_0_1 = var_0_1 + 1
			var_0_0 = 0
		end
	end,
	function(arg_10_0)
		arg_10_0.camera:enable()

		var_0_1 = var_0_1 + 1
	end,
	[6] = function(arg_11_0)
		if not Store:isBusy() and not arg_11_0.fade:isRunning() then
			system.noSave = true
			arg_11_0.dialogclosed = true

			if not system.exitEvent then
				local var_11_0 = {}

				if system.uisave_savefailed then
					table.insert(var_11_0, "sys_err_menuSaveStoreFail")

					system.uisave_savefailed = nil
				end

				if system.floatingpoint_deletefailed then
					table.insert(var_11_0, "sys_error_resumeStoreFail")

					system.floatingpoint_deletefailed = nil
				end

				if not table_isEmpty(var_11_0) then
					arg_11_0.dialogclosed = false

					local function var_11_1(arg_12_0)
						GUI.disabled = false

						CloverPadUI:EnableUserInput(true, "run")
						arg_11_0.camera:enable()
						arg_11_0.dialog:setText(Localization.getText(var_11_0[arg_12_0]))
						arg_11_0.dialog:showDialog(nil, function()
							GUI.disabled = true

							Tween:wait(0.5):callback(function()
								if arg_12_0 == #var_11_0 then
									arg_11_0.dialogclosed = true
								else
									var_11_1(arg_12_0 + 1)
								end
							end):start()
						end)
					end

					var_11_1(1)
				end
			end

			var_0_1 = var_0_1 + 1
		end
	end,
	[7] = function(arg_15_0)
		if arg_15_0.dialogclosed then
			system.checkClock("fadeOut")

			if arg_15_0.fadeOutFunc then
				arg_15_0.fadeOutFunc()
			end

			if HOST_PLATFORM_IS_WINDOWS then
				system.noSave = false

				if debug_store.noBlackScreen then
					arg_15_0.fade:disable()

					GUI.disabled = false

					CloverPadUI:EnableUserInput(true, "run")
					GUI:focusElement(system.focusedElement)

					system.focusedElement = nil
				else
					arg_15_0.fade:In(0, nil, function()
						GUI.disabled = false

						CloverPadUI:EnableUserInput(true, "run")
						GUI:focusElement(system.focusedElement)

						system.focusedElement = nil
					end)
				end

				var_0_1 = var_0_3
			else
				var_0_1 = var_0_4
			end
		end
	end
}
local var_0_6

function sys_boot.update(arg_17_0, arg_17_1)
	Pad.Update(arg_17_1)
	CloverTask.Update(arg_17_1)

	if var_0_6 and var_0_6 > 0 then
		var_0_6 = var_0_6 - 1

		return
	end

	bootTime = bootTime + arg_17_1 * var_0_0

	if var_0_5[var_0_1] then
		var_0_5[var_0_1](arg_17_0)
	end
end

function sys_boot.stop(arg_18_0)
	mcp.finishTransition()
end

function sys_boot.getInstance(arg_19_0)
	return sys_boot.instance
end

function sys_boot.fadeOut(arg_20_0, arg_20_1, ...)
	if var_0_1 <= var_0_3 then
		GUI.disabled = true
		system.focusedElement = GUI.focusedElement

		GUI:focusElement(nil)
		system.save_setting()

		if not Store.isSaveRequested then
			system.noSave = true
		end

		var_0_1 = var_0_3 + 1

		if debug_store.noBlackScreen then
			arg_20_0.fade:disable()

			if system.thumbcur then
				system.thumbcur:disable()
			end

			if arg_20_1 then
				arg_20_1(unpack({
					...
				}))
			end
		else
			local var_20_0 = {
				...
			}

			function arg_20_0.fadeOutFunc()
				if arg_20_1 then
					arg_20_1(unpack(var_20_0))
				end
			end

			if arg_20_0.fade:isOutEnd() then
				-- block empty
			else
				arg_20_0.fade:Out(system.delayFadeOut or 0, nil, nil)
			end
		end
	end
end

function sys_boot.loadFont(arg_22_0, arg_22_1, arg_22_2)
	if not arg_22_1 and HOST_PLATFORM_IS_WINDOWS then
		if IDE_CURRENT_PROJECT_DIR then
			arg_22_1 = IDE_CURRENT_PROJECT_DIR .. "/fonts/" .. arg_22_2
		elseif os.getenv("CLOVER_DEV_VIEWER") then
			arg_22_1 = os.getenv("CLOVER_REPOSITORY_MENU") .. "/fonts/" .. arg_22_2
		else
			arg_22_1 = Application.getDataPath("../../../fonts/" .. arg_22_2)
		end
	end

	if not arg_22_1 then
		return nil
	end

	local var_22_0 = createFontFromPath(arg_22_1)

	var_22_0:link()

	return var_22_0
end

function sys_boot.isUpdateMain(arg_23_0)
	return var_0_1 == var_0_3
end
