require("/scripts/core/core.lua")
require("/scripts/Store_myplay_data.lua")
require("/scripts/app/clover_scene.lua")
require("/scripts/app/clover_task.lua")
require("/scripts/app/clover_util.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_transition.lua")
require("/scripts/app/clover_elements.lua")
require("/scripts/app/clover_character.lua")
require("/scripts/app/clover_event.lua")
require("/scripts/app/clover_nodeanim.lua")
require("/scripts/app/clover_const.lua")
require("/scripts/app/clover_layout.lua")
require("/scripts/app/clover_bg.lua")

local var_0_0 = {
	LUIGI_Z_INDEX = 16,
	DEMO_IN_Z_INDEX = 0,
	DEFAULT_Z_INDEX = 0,
	MYPLAY_ONMENU_Z_INDEX = 35,
	HIT_Z_INDEX = 10,
	MYPLAY_Z_INDEX = 20,
	Phase = {
		Auto = 1,
		Myplay = 2,
		None = 0
	}
}
local var_0_1 = {}

CloverDemo = {
	Init = function(arg_1_0)
		local var_1_0 = CloverDemo

		var_1_0.is_enable = true
		var_1_0.running_cancel = false
		var_1_0.scene = CloverScene.AddResource("demo_scene", arg_1_0)

		CloverTask.Register("demo_scene", CloverDemo.Update)

		if var_1_0.is_boot_disable then
			var_1_0.is_enable = false

			CloverTask.Sleep("demo_scene")
		end

		var_1_0.state = CloverState.new()

		var_1_0.state:add("wait_cancel", var_1_0.StateWaitCanceldemo)
		var_1_0.state:add("wait", var_1_0.StateWaitAutodemo)
		var_1_0.state:add("ready", var_1_0.StateReadydemo)
		var_1_0.state:add("next_auto", var_1_0.StateNextAutodemo)
		var_1_0.state:add("play_auto", var_1_0.StatePlayAutodemo)
		var_1_0.state:add("run_auto", var_1_0.StateRunAutodemo)
		var_1_0.state:add("next_myplay", var_1_0.StateNextMyplaydemo)
		var_1_0.state:add("play_myplay", var_1_0.StatePlayMyplaydemo)
		var_1_0.state:add("run_myplay", var_1_0.StateRunMyplaydemo)
		var_1_0.state:add("switch_demo", var_1_0.StateSwitchDemo)
		var_1_0.state:add("play_display", var_1_0.StatePlayDisplaydemo)
		var_1_0.state:add("cancel", var_1_0.StateCanceldemo)
		var_1_0.state:change("wait")

		var_1_0.demo_phase = var_0_0.Phase.None

		var_1_0.ResetRequest()

		var_1_0.mario = nil
		var_1_0.luigi = nil
		var_1_0.hit_anim = nil
		var_1_0.event = CloverEvent.new()

		var_0_1.EnableAutoplayMark(false)

		var_1_0.bootDemo = false
		var_1_0.bootAborted = false

		if store.autoplay and store.autoplay.running then
			if PREVIOUS_COMMAND == "super-play" then
				var_1_0.bootDemo = true
				var_1_0.bootAborted = TITLE_RETURN_CODE == 1

				print("return from demo ...")
				var_0_1.ToDemoState()
				system.waitBootFade("demo", true)
			else
				system.waitBootFade("demo", true)
				var_1_0.state:change("wait_cancel")
			end
		end

		if var_1_0.bootDemo == false or var_1_0.bootAborted then
			store.autoplay.myplayIndex = 1
		end

		var_1_0.open_resumemenu = false
		var_1_0.cursor_no_move = false
	end,
	Update = function(arg_2_0)
		local var_2_0 = CloverDemo

		if not var_2_0.is_enable and var_2_0.demo_phase == var_0_0.Phase.None then
			CloverTask.Sleep("demo_scene")

			return
		end

		var_2_0.state:update(arg_2_0)

		if var_2_0.mario then
			var_2_0.mario:update(arg_2_0)
		end

		if var_2_0.luigi then
			var_2_0.luigi:update(arg_2_0)
		end

		if var_2_0.hit_anim then
			var_2_0.hit_anim:update(arg_2_0)
		end
	end,
	IsPlayingAutoDemo = function()
		return CloverDemo.demo_phase == var_0_0.Phase.Auto
	end,
	IsPlayingMyplayDemo = function()
		return CloverDemo.demo_phase == var_0_0.Phase.Myplay
	end,
	ToAutodemo = function()
		CloverDemo.forceAutoDemoRequest = true
	end,
	ToMyplaydemo = function()
		CloverDemo.forceMyplayDemoRequest = true
	end,
	Enable = function()
		CloverDemo.is_enable = true

		if CloverDemo.demo_phase == var_0_0.Phase.None then
			CloverTask.Awake("demo_scene")
		end
	end,
	Disable = function()
		if CloverDemo.is_enable == nil then
			CloverDemo.is_boot_disable = true

			return
		end

		CloverDemo.is_enable = false

		if CloverDemo.demo_phase == var_0_0.Phase.None then
			CloverTask.Sleep("demo_scene")
		end
	end,
	ResetRequest = function()
		CloverDemo.forceAutoDemoRequest = false
		CloverDemo.forceMyplayDemoRequest = false
		CloverDemo.displayDemo = false
	end,
	StateWaitCanceldemo = function(arg_10_0)
		if system.TextureIsLoaded() or arg_10_0.passed_time > CloverConst.System.TIMEOUT_SECOND then
			system.waitBootFade("demo", false)
			var_0_1.CancelDemo()
			arg_10_0:set_next("wait")
		end
	end,
	StateWaitAutodemo = function(arg_11_0)
		local var_11_0 = CloverDemo

		if arg_11_0:is_first() then
			var_11_0.demo_phase = var_0_0.Phase.None
		end

		local var_11_1 = false

		if var_11_0.forceMyplayDemoRequest then
			if MyplayData.IsEmpty() == false and store.setting.myplayDemo then
				var_11_1 = true
			end

			if MyplayData.IsEmpty() then
				var_11_0.forceMyplayDemoRequest = false
			end
		end

		if var_11_0.forceAutoDemoRequest and store.setting.autoplayDemo then
			var_11_1 = true
		end

		if CloverPadUI.no_input_time > CloverConst.Demo.START_SECONDS then
			if MyplayData.IsEmpty() == false and store.setting.myplayDemo then
				var_11_1 = true
			elseif store.setting.autoplayDemo then
				var_11_1 = true
			end
		end

		if var_11_1 then
			CloverElements.resumedummy:startDemo()
			var_0_1.ToDemoState()
		end
	end,
	StateReadydemo = function(arg_12_0)
		local var_12_0 = CloverDemo
		local var_12_1 = var_12_0.event

		if arg_12_0:is_first() then
			var_12_0.scene:Link()
			var_12_0.scene:AddNode()
			var_12_0.scene:SetVisible(false)
			var_12_1:reset()
		end

		local var_12_2 = {
			function()
				if var_12_0.scene:IsReady() then
					var_12_0.event:next_index()

					if var_12_0.mario == nil then
						var_12_0.mario = CloverMario.new(var_12_0.scene:GetNode(), "mario")
					end

					if var_12_0.luigi == nil then
						var_12_0.luigi = CloverCharacter.new(var_12_0.scene:GetNode(), "luigi")
					end

					if var_12_0.hit_anim == nil then
						var_12_0.hit_anim = CloverNodeAnim.new(var_12_0.scene:GetNode(), "hit", "Anim_Hit")
					end

					var_12_0.scene:SetVisible(true)
					var_12_0.mario:SetVisible(false)
					var_12_0.mario:SetLocalPosition(0, CloverConst.Demo.AUTOPLAY_START_MARIO_Y)
					var_12_0.mario:SetWorldZIndex(var_0_0.DEFAULT_Z_INDEX)
					var_12_0.luigi:SetVisible(false)
					var_12_0.luigi:SetLocalPosition(CloverConst.Demo.DISPLAY_CHANGE_LUIGI_X, CloverConst.Demo.DISPLAY_CHANGE_LUIGI_Y)
					var_12_0.luigi:SetWorldZIndex(var_0_0.LUIGI_Z_INDEX)
					var_12_0.hit_anim:Reset()
				end
			end,
			function()
				system.waitBootFade("demo", false)

				if not sys_boot:isUpdateMain() then
					return
				end

				CloverTransition:StartTransition(function()
					local var_15_0 = CloverElements.menubar

					if var_15_0:elementIsFocused() then
						var_15_0:deactivate()
					end

					if not var_12_0.bootDemo then
						var_0_1.SetTitleSort()
					end
				end, nil, "AutoPlayOptionMenuIn", var_12_0.bootDemo)

				if var_12_0.bootDemo then
					CloverBG.Switch("demo", 0)
				else
					CloverBG.Switch("demo", CloverConst.TRANSITION.DURATION)
				end

				var_12_1:next_index()
			end,
			function()
				if var_12_0.bootDemo then
					var_12_0.mario:SetVisible(true)
					var_12_0.mario:LookRight()

					local var_16_0, var_16_1 = CloverElements.gametitlelist.current:getWorldPosition()
					local var_16_2, var_16_3 = var_12_0.mario:GetWorldPosition()

					var_12_0.mario:SetWorldPosition(var_16_0, var_16_3)
				else
					var_12_1:wait(CloverConst.Demo.MARIO_IN_WAIT_SECONDS)
				end

				var_12_1:next_index()
			end,
			function()
				if not var_12_0.bootDemo then
					var_12_0.mario:SetVisible(true)
					var_12_0.mario:LookLeft()
					var_12_0.mario:SetWorldZIndex(var_0_0.DEMO_IN_Z_INDEX)
					var_12_0.mario:OneshotAnim("in", false, false)
				end

				var_12_1:next_index()
			end,
			function()
				if not CloverTransition.is_running and var_12_0.mario:IsStay() then
					if not var_12_0.bootAborted then
						var_12_1:wait(0.5)
					end

					var_12_1:next_index()
				end
			end,
			function()
				local var_19_0 = var_0_1.GetStartState()

				arg_12_0:set_next(var_19_0)
				var_12_0.ResetRequest()
				var_12_1:next_index()
			end
		}

		var_12_1:update(var_12_2, arg_12_0.dt)
	end,
	StateNextAutodemo = function(arg_20_0)
		local var_20_0 = CloverDemo
		local var_20_1 = var_20_0.event
		local var_20_2 = var_20_0.mario

		if arg_20_0:is_first() then
			var_20_1:reset()
		end

		if var_0_1.CheckCancelImput() then
			arg_20_0:set_next("cancel")

			return
		end

		local var_20_3 = {
			function()
				var_20_1:wait(0.5)
				var_20_1:next_index()
			end,
			function()
				CloverPadAuto:inputRequest("right")

				local var_22_0, var_22_1 = var_20_2:GetWorldPosition()

				if var_0_1.IsRightEdgeSelect() then
					var_20_2:Walk(var_22_0, nil, nil, true)
				else
					local var_22_2, var_22_3 = CloverElements.gametitlelist.current:getWorldPosition()
					local var_22_4, var_22_5 = var_20_2:GetWorldPosition()

					if var_22_4 <= var_22_2 then
						var_20_2:Walk(var_22_4 + 256)
					else
						var_20_2:Walk(var_22_4 - 256)
					end
				end

				var_20_2:SetKeepPose(true)
				var_20_1:next_index()
			end,
			function()
				var_20_1:wait(0.08)
				var_20_1:next_index()
			end,
			function()
				arg_20_0:set_next("play_auto")
				var_20_1:next_index()
			end
		}

		var_20_1:update(var_20_3, arg_20_0.dt)
	end,
	StatePlayAutodemo = function(arg_25_0)
		local var_25_0 = CloverDemo
		local var_25_1 = var_25_0.event

		if arg_25_0:is_first() then
			var_25_0.hit_anim:Reset()
			var_25_1:reset()
		end

		if var_0_1.CheckCancelImput() then
			arg_25_0:set_next("cancel")

			return
		end

		local var_25_2 = {
			function()
				local var_26_0, var_26_1 = CloverElements.gametitlelist.current:getWorldPosition()

				if var_25_0.mario:IsNeedWalk(var_26_0) then
					if var_25_0.mario:IsWalk() then
						var_25_0.mario:UpdateWalkTarget(var_26_0)
					else
						var_25_0.mario:Walk(var_26_0)
					end

					var_25_0.mario:SetKeepPose(false)
				elseif var_25_0.mario:IsWalk() then
					var_25_0.mario:UpdateWalkTarget(var_26_0)
				end

				var_25_1:next_index()
			end,
			function()
				if var_25_0.mario:IsStay() then
					var_25_1:next_index()
				end
			end,
			function()
				if var_0_1.IsEdgeSelect() then
					var_25_1:wait(0.25)
				end

				var_25_1:next_index()
			end,
			function()
				var_25_0.mario:SetWorldZIndex(var_0_0.HIT_Z_INDEX)
				var_25_0.mario:OneshotAnim("hit_title", false, false)
				var_25_0.hit_anim:Bind(CloverElements.gametitlelist.current)
				var_25_0.hit_anim:Bind(CloverElements.gametitlelist.cursor_elements)
				var_25_0.hit_anim:ChangeAnim("hit_title", false)
				var_25_1:next_index()
			end,
			function()
				var_25_1:wait(0.25)
				var_25_1:next_index()
			end,
			function()
				arg_25_0:set_next("run_auto")
				var_25_1:next_index()
			end
		}

		var_25_1:update(var_25_2, arg_25_0.dt)
	end,
	StateRunAutodemo = function(arg_32_0)
		local var_32_0 = CloverDemo
		local var_32_1 = var_32_0.event

		if arg_32_0:is_first() then
			store.autoplay.current_game_code = system.game_card.gameinfo.game_code

			if not store.autoplay.start_game_code then
				store.autoplay.start_game_code = store.autoplay.current_game_code
			end

			store.autoplay.displayOptionCount = store.autoplay.displayOptionCount - 1

			var_32_1:reset()
		end

		local var_32_2 = {
			function()
				if var_32_0.hit_anim:isAnimStopped() then
					var_32_0.hit_anim:Reset()
					system.game_card:expandAnimation()

					store.autoplay.to_next_index = true

					system.run_gamedemoplay(system.game_card.gameinfo.game_code)
					Main:stopMainBGM()
					var_32_1:next_index()
				end
			end,
			function()
				return
			end
		}

		var_32_1:update(var_32_2, arg_32_0.dt)

		if arg_32_0.passed_time > 2 then
			system.game_card:expandAnimation_reset()
			system.game_card:setCursor()
			GUI:focusElement(nil)
			GUI:focusElement(system.game_card)

			var_32_0.bootDemo = true

			var_0_1.ToDemoState()
			Main:playMainBGM()
		end
	end,
	StateNextMyplaydemo = function(arg_35_0)
		local var_35_0 = CloverDemo
		local var_35_1 = var_35_0.event
		local var_35_2 = var_35_0.mario

		if arg_35_0:is_first() then
			local var_35_3 = var_0_1.GameCodeToIndex(system.game_card.gameinfo.game_code)
			local var_35_4 = var_0_1.GameCodeToIndex(var_0_1.GetNextMyplayTitle())

			arg_35_0.local_value.cursor_left = true

			local var_35_5 = #CloverElements.gametitlelist.elementArray / 2
			local var_35_6 = var_35_4 - var_35_3

			var_35_0.cursor_no_move = false
			arg_35_0.local_value.move_index = 0

			if var_35_6 == 0 then
				arg_35_0.local_value.to_next_state = true
				var_35_0.cursor_no_move = true
			elseif var_35_6 > 0 then
				if var_35_6 < var_35_5 then
					arg_35_0.local_value.cursor_left = false
				else
					arg_35_0.local_value.cursor_left = true
				end
			elseif var_35_6 < -var_35_5 then
				arg_35_0.local_value.cursor_left = false
			else
				arg_35_0.local_value.cursor_left = true
			end

			if not var_35_0.cursor_no_move then
				if arg_35_0.local_value.cursor_left then
					if var_35_6 < 0 then
						arg_35_0.local_value.move_index = var_35_6
					else
						arg_35_0.local_value.move_index = var_35_6 - #CloverElements.gametitlelist.elementArray
					end
				elseif var_35_6 > 0 then
					arg_35_0.local_value.move_index = var_35_6
				else
					arg_35_0.local_value.move_index = var_35_6 + #CloverElements.gametitlelist.elementArray
				end
			end

			arg_35_0.local_value.repeat_time = 0
			arg_35_0.local_value.repeat_interval = CloverConst.Demo.REPEAT_DELAY

			var_35_1:reset()
		end

		if var_0_1.CheckCancelImput() then
			arg_35_0:set_next("cancel")

			return
		end

		local var_35_7 = {
			function()
				var_35_1:wait(0.5)
				var_35_1:next_index()
			end,
			function()
				local var_37_0 = var_0_1.GetSelectLine()

				if arg_35_0.local_value.cursor_left then
					var_35_2:LookLeft()
				else
					var_35_2:LookRight()
				end

				local var_37_1 = false
				local var_37_2 = false
				local var_37_3 = CloverLayout.GetSelectableTitleX(var_37_0)

				if var_37_0 == 1 then
					if arg_35_0.local_value.move_index > 3 then
						var_37_3 = 0
					elseif arg_35_0.local_value.move_index > 0 then
						local var_37_4 = var_37_0 + arg_35_0.local_value.move_index

						var_37_3 = CloverLayout.GetSelectableTitleX(var_37_4)
					end
				elseif var_37_0 == 2 then
					if arg_35_0.local_value.move_index > 2 then
						var_37_3 = 0
					elseif arg_35_0.local_value.move_index > 0 then
						local var_37_5 = var_37_0 + arg_35_0.local_value.move_index

						var_37_3 = CloverLayout.GetSelectableTitleX(var_37_5)
					else
						var_37_3 = CloverLayout.GetSelectableTitleX(1)
					end
				elseif var_37_0 == 3 then
					if arg_35_0.local_value.move_index < -2 then
						var_37_3 = 0
					elseif arg_35_0.local_value.move_index < 0 then
						local var_37_6 = var_37_0 + arg_35_0.local_value.move_index

						var_37_3 = CloverLayout.GetSelectableTitleX(var_37_6)
					else
						var_37_3 = CloverLayout.GetSelectableTitleX(4)
					end
				elseif arg_35_0.local_value.move_index < -3 then
					var_37_3 = 0
				elseif arg_35_0.local_value.move_index < 0 then
					local var_37_7 = var_37_0 + arg_35_0.local_value.move_index

					var_37_3 = CloverLayout.GetSelectableTitleX(var_37_7)
				end

				var_35_2:Walk(var_37_3)
				var_35_2:SetKeepPose(true)
				var_35_1:next_index()
			end,
			function()
				if arg_35_0.local_value.repeat_time <= 0 then
					if arg_35_0.local_value.cursor_left then
						CloverPadAuto:inputRequest("left")
					else
						CloverPadAuto:inputRequest("right")
					end

					arg_35_0.local_value.repeat_time = arg_35_0.local_value.repeat_interval
					arg_35_0.local_value.repeat_interval = CloverConst.Demo.REPEAT_RATE
				else
					arg_35_0.local_value.repeat_time = arg_35_0.local_value.repeat_time - arg_35_0.dt
				end

				if system.game_card.gameinfo.game_code == var_0_1.GetNextMyplayTitle() then
					var_35_1:next_index()
				end
			end,
			function()
				var_35_1:wait(0.125)
				var_35_1:next_index()
			end,
			function()
				arg_35_0.local_value.to_next_state = true
			end
		}

		if arg_35_0.local_value.to_next_state then
			arg_35_0:set_next("play_myplay")
		else
			var_35_1:update(var_35_7, arg_35_0.dt)
		end
	end,
	StatePlayMyplaydemo = function(arg_41_0)
		local var_41_0 = CloverDemo
		local var_41_1 = var_41_0.event
		local var_41_2 = var_41_0.mario

		if arg_41_0:is_first() then
			var_41_1:reset()
		end

		if var_0_1.CheckCancelImput() then
			arg_41_0:set_next("cancel")

			return
		end

		local var_41_3 = {
			function()
				if var_41_0.cursor_no_move == false then
					local var_42_0, var_42_1 = CloverElements.gametitlelist.current:getWorldPosition()

					if var_41_2:IsWalk() then
						var_41_2:UpdateWalkTarget(var_42_0)
					else
						var_41_2:Walk(var_42_0)
					end
				end

				var_41_1:next_index()
			end,
			function()
				if var_41_0.mario:IsStay() then
					var_41_1:next_index()
				else
					local var_43_0, var_43_1 = CloverElements.gametitlelist.current:getWorldPosition()

					var_41_2:UpdateWalkTarget(var_43_0)
				end
			end,
			function()
				CloverPadAuto:inputRequest("down")

				var_41_0.open_resumemenu = true

				var_41_1:wait(0.125)
				var_41_1:next_index()
			end,
			function()
				local var_45_0 = var_0_1.GetNextMyplaySuspentionPointPosX()
				local var_45_1, var_45_2 = var_41_2:GetWorldPosition()
				local var_45_3 = var_45_1

				if var_45_0 < var_45_1 then
					var_45_3 = var_45_3 - CloverConst.Demo.MYPLAY_MARIO_SHIFT_X
				else
					var_45_3 = var_45_3 + CloverConst.Demo.MYPLAY_MARIO_SHIFT_X
				end

				var_41_2:SetWorldZIndex(var_0_0.MYPLAY_Z_INDEX)
				var_41_2:Ride(var_45_3)
				var_41_1:wait(0.5)
				var_41_1:next_index()
			end,
			function()
				var_41_2:SetWorldZIndex(var_0_0.MYPLAY_ONMENU_Z_INDEX)
				var_41_1:next_index()
			end,
			function()
				if var_41_2:IsStay() then
					var_41_1:wait(0.25)
					var_41_1:next_index()
				end
			end,
			function()
				local var_48_0 = var_0_1.GetNextMyplaySuspentionPointPosX()
				local var_48_1, var_48_2 = var_41_2:GetWorldPosition()
				local var_48_3 = var_48_0

				if var_48_1 < var_48_0 then
					var_48_3 = var_48_3 - CloverConst.Demo.MYPLAY_MARIO_SUSPENTION_SHIFT_X
				else
					var_48_3 = var_48_3 + CloverConst.Demo.MYPLAY_MARIO_SUSPENTION_SHIFT_X
				end

				var_41_2:Walk(var_48_3, var_48_0, 0.15)
				var_41_1:next_index()
			end,
			function()
				if var_41_2:IsStay() then
					var_41_1:wait(0.25)
					var_41_1:next_index()
				end
			end,
			function()
				local var_50_0 = var_0_1.GetNextMyplaySuspentionPointPosX()
				local var_50_1, var_50_2 = var_41_2:GetWorldPosition()

				if var_50_1 < var_50_0 then
					var_41_2:Spin(CloverConst.Demo.MYPLAY_MARIO_SPIN_MOVE_SPEED)
				else
					var_41_2:Spin(-CloverConst.Demo.MYPLAY_MARIO_SPIN_MOVE_SPEED)
				end

				var_41_1:wait(CloverConst.Demo.MYPLAY_MARIO_SPIN_AFTER_SECONDS)
				var_41_1:next_index()
			end,
			function()
				arg_41_0:set_next("run_myplay")
				var_41_1:next_index()
			end
		}

		var_41_1:update(var_41_3, arg_41_0.dt)
	end,
	StateRunMyplaydemo = function(arg_52_0)
		local var_52_0 = CloverDemo
		local var_52_1 = var_52_0.event

		if arg_52_0:is_first() then
			local var_52_2 = var_0_1.GetNextMyplayTitle()
			local var_52_3 = var_0_1.GetNextMyplaySuspentionPointIndex()

			var_0_1.IncrementMyplayIndex()

			store.autoplay.displayOptionCount = store.autoplay.displayOptionCount - 1

			local var_52_4 = CloverElements.resumemenu:getCard(var_52_3)

			arg_52_0.local_value.suspention_element = var_52_4

			arg_52_0.local_value.suspention_element:expandAnimation()

			local var_52_5 = var_52_4.resumeinfo:getRollbackDataPath()
			local var_52_6 = true

			system.run_gamemyplay(var_52_2, var_52_5, var_52_6)
			Main:stopMainBGM()
			var_52_1:reset()
		end

		local var_52_7 = {
			function()
				return
			end
		}

		var_52_1:update(var_52_7, arg_52_0.dt)

		if arg_52_0.passed_time > 2 then
			if var_52_0.open_resumemenu then
				arg_52_0.local_value.suspention_element:expandAnimation_reset()
				CloverElements.resumemenu:deactivate()

				var_52_0.open_resumemenu = false
			end

			system.game_card:setCursor()
			GUI:focusElement(nil)
			GUI:focusElement(system.game_card)
			Main:playMainBGM()

			var_52_0.bootDemo = true

			var_0_1.ToDemoState()
		end
	end,
	StateSwitchDemo = function(arg_54_0)
		local var_54_0 = CloverDemo
		local var_54_1 = var_54_0.event

		if arg_54_0:is_first() then
			var_54_1:reset()

			if var_54_0.demo_phase == var_0_0.Phase.Auto then
				var_54_0.demo_phase = var_0_0.Phase.Myplay
				store.autoplay.demo_phase = var_0_0.Phase.Myplay

				var_0_1.FinishAutodemo()
			else
				var_54_0.demo_phase = var_0_0.Phase.Auto
				store.autoplay.demo_phase = var_0_0.Phase.Auto
			end

			var_0_1.EnableAutoplayMark(false)
		end

		local var_54_2 = {
			function()
				CloverTransition:StartTransition(function()
					var_0_1.EnableAutoplayMark(true)
					var_0_1.SetTitleSort()
				end, nil, "AutoPlayOptionMenuIn", false)
				var_54_1:next_index()
			end,
			function()
				if not CloverTransition.is_running then
					var_54_1:wait(CloverConst.Demo.SWITCH_DEMO_WAIT_SECONDS)
					var_54_1:next_index()
				end
			end,
			function()
				if var_54_0.demo_phase == var_0_0.Phase.Auto then
					arg_54_0:set_next("play_auto")
				else
					arg_54_0:set_next("next_myplay")
				end

				var_54_1:next_index()
			end
		}

		var_54_1:update(var_54_2, arg_54_0.dt)
	end,
	StatePlayDisplaydemo = function(arg_59_0)
		local var_59_0 = CloverDemo
		local var_59_1 = var_59_0.event
		local var_59_2 = var_59_0.mario
		local var_59_3 = var_59_0.luigi

		if arg_59_0:is_first() then
			var_59_1:reset()
		end

		if var_0_1.CheckCancelImput() then
			arg_59_0:set_next("cancel")

			return
		end

		local var_59_4 = {
			function()
				CloverPadAuto:inputRequest("up")
				var_59_1:wait(0.25)
				var_59_1:next_index()
			end,
			function()
				var_59_2:OneshotAnim("look_up", true, true)
				var_59_1:next_index()
			end,
			function()
				if GUI.focusedElement ~= CloverElements.displayoptionButton then
					CloverPadAuto:inputRequest("left")
					var_59_1:wait(0.25)
				else
					var_59_1:next_index()
				end
			end,
			function()
				var_59_3:SetLocalPosition(CloverConst.Demo.DISPLAY_CHANGE_LUIGI_X, CloverConst.Demo.DISPLAY_CHANGE_LUIGI_Y)
				var_59_3:SetVisible(true)

				local var_63_0, var_63_1 = CloverElements.displayoptionButton:getWorldPosition()

				var_59_3:Walk(var_63_0)
				var_59_1:next_index()
			end,
			function()
				if var_59_3:IsStay() then
					var_59_1:next_index()
				end
			end,
			function()
				var_59_3:Jump()
				var_59_0.hit_anim:Bind(CloverElements.displayoptionButton)
				var_59_0.hit_anim:ChangeAnim("hit_option", false)
				var_59_1:wait(0.25)
				var_59_1:next_index()
			end,
			function()
				CloverPadAuto:inputRequest(Pad.validate_btn)
				var_59_1:wait(2)
				var_59_1:next_index()
			end,
			function()
				CloverPadAuto:inputRequest("right")
				var_59_1:wait(1)
				var_59_1:next_index()
			end,
			function()
				CloverPadAuto:inputRequest(Pad.validate_btn)
				var_59_1:wait(1)
				var_59_1:next_index()
			end,
			function()
				CloverPadAuto:inputRequest(Pad.cancel_btn)
				var_59_1:wait(1)
				var_59_1:next_index()
			end,
			function()
				var_59_3:Walk(-CloverConst.Demo.DISPLAY_CHANGE_LUIGI_X)
				var_59_1:wait(CloverConst.Demo.DISPLAY_CURSOR_END_SECONDS)
				var_59_1:next_index()
			end,
			function()
				CloverPadAuto:inputRequest("down")
				var_59_1:wait(CloverConst.Demo.DISPLAY_MARIO_LOOKDOWN_SECONDS)
				var_59_1:next_index()
			end,
			function()
				var_59_2:OneshotAnim("wait", false, true)
				var_59_1:next_index()
			end,
			function()
				if var_59_3:IsStay() then
					var_59_3:SetVisible(false)
					CloverElements.gametitlelabel:enable()
					var_59_1:wait(0.5)
					var_59_1:next_index()
				end
			end,
			function()
				arg_59_0:set_next(var_0_1.GetStartState_OnlyDemo())
				var_0_1.ResetDisplayCount()
			end
		}

		var_59_1:update(var_59_4, arg_59_0.dt)
	end,
	StateCanceldemo = function(arg_75_0)
		local var_75_0 = CloverDemo
		local var_75_1 = var_75_0.event

		if arg_75_0:is_first() then
			var_75_1:reset()

			CloverDemo.running_cancel = true
		end

		local var_75_2 = {
			function()
				var_75_0.hit_anim:Reset()
				CloverTransition:StartTransition(function()
					system.cursor:cursorHide()
					CloverElements.gametitlelist:activate()
					var_0_1.CancelDemo()
				end, nil, "AutoPlayOut")
				CloverTransition:setRelative()
				CloverBG.Switch("default", CloverConst.TRANSITION.DURATION)

				if CloverElements.displayoption:elementIsFocused() then
					Main:toHomeMenu()
				end

				var_75_0.cursor_no_move = false

				var_75_1:next_index()
			end,
			function()
				var_75_1:wait(0.2)
				var_75_1:next_index()
			end,
			function()
				if var_75_0.open_resumemenu then
					CloverElements.resumemenu:deactivate()
					CloverDemo.Enable()

					var_75_0.open_resumemenu = false
				end

				var_75_1:next_index()
			end,
			function()
				var_75_0.mario:Exit()
				var_75_0.luigi:Exit()
				var_75_1:next_index()
			end,
			function()
				if not CloverTransition.is_running then
					arg_75_0:set_next("wait")

					CloverDemo.running_cancel = false

					CloverPadUI:EnableUserInput(true, "demo")
					CloverPadUI:reset()
					var_75_0.scene:SetVisible(false)

					var_75_0.bootDemo = false
					var_75_0.bootAborted = false

					var_75_1:next_index()
				end
			end
		}

		var_75_1:update(var_75_2, arg_75_0.dt)
	end
}

if REED_DEBUG then
	function GetCloverDemoConst()
		return var_0_0
	end
end

function var_0_1.AutoPlayCursorSave(arg_83_0)
	if not store.setting then
		store.setting = {}
	end

	if not store.setting.display then
		store.setting.display = "keep-aspect-ratio"
	end

	if not store.autoplay then
		store.autoplay = {}
	end

	local var_83_0 = store.autoplay.backup or {}

	if not store.autoplay.running then
		store.autoplay.running = true
		store.autoplay.demo_phase = arg_83_0
		var_83_0.game_code = system.game_card.gameinfo.game_code
		var_83_0.displayoption = store.setting.display
		var_83_0.gametitlelistDX = store.gametitlelistDX
		store.autoplay.backup = var_83_0

		if store.autoplay.displayOption ~= store.setting.display then
			if store.autoplay.displayOption then
				debugLabelPrint(("chaged?? %s, %s"):format(store.autoplay.displayOption, store.setting.display))
			end

			var_0_1.ResetDisplayCount()
		end
	end
end

function var_0_1.CancelDemo()
	if not store.autoplay then
		store.autoplay = {}
	end

	var_0_1.EnableAutoplayMark(false)

	if CloverElements.displayoption:elementIsFocused() then
		Main:toHomeMenu()
	end

	if CloverElements.menubar:elementIsFocused() then
		CloverElements.menubar:deactivate()
	end

	local var_84_0 = store.autoplay.backup
	local var_84_1

	if var_84_0 then
		store.gametitlelistDX = var_84_0.gametitlelistDX
		CloverElements.gametitlelist.firstIndex = nil
		var_84_1 = var_84_0.game_code
	end

	var_0_1.FinishAutodemo()
	CloverElements.gametitlelist:setSort(store.sortrule, true, var_84_1)

	store.autoplay.myplayIndex = 1

	if var_84_0 then
		CloverElements.gametitlelist.menu.current = CloverElements.gametitlelist

		local var_84_2 = table_find_if(CloverElements.gametitlelist.elementArray, function(arg_85_0)
			return arg_85_0.gameinfo.game_code == var_84_0.game_code
		end)
		local var_84_3 = CloverElements.gametitlelist.elementArray[var_84_2]

		if CloverElements.menubar:elementIsFocused() or CloverElements.gametitlelist:elementIsFocused() then
			GUI:focusElement(var_84_3)
		end

		CloverElements.gametitlelist.current = var_84_3
		store.setting.display = var_84_0.displayoption
		store.autoplay.running = false
		store.autoplay.demo_phase = var_0_0.Phase.None
	end

	store.autoplay.backup = nil

	if store.setting.burn_inPreviention and HOST_PLATFORM_IS_LINUX then
		mcp.set_dimming_timer(CloverConst.System.DIMMING_SECONDS)
	end
end

function var_0_1.ToDemoState()
	local var_86_0 = CloverDemo

	if var_86_0.bootDemo then
		var_86_0.demo_phase = store.autoplay.demo_phase
	elseif MyplayData.IsEmpty() then
		var_86_0.demo_phase = var_0_0.Phase.Auto
	elseif var_86_0.forceMyplayDemoRequest and store.setting.myplayDemo then
		var_86_0.demo_phase = var_0_0.Phase.Myplay
	elseif var_86_0.forceAutoDemoRequest and store.setting.autoplayDemo then
		var_86_0.demo_phase = var_0_0.Phase.Auto
	elseif store.setting.myplayDemo then
		var_86_0.demo_phase = var_0_0.Phase.Myplay
	elseif store.setting.autoplayDemo then
		var_86_0.demo_phase = var_0_0.Phase.Auto
	else
		assert(false)
	end

	var_86_0.state:set_next("ready")
	CloverPadUI:EnableUserInput(false, "demo")
	var_0_1.AutoPlayCursorSave(var_86_0.demo_phase)

	if store.autoplay.displayOptionCount <= 0 then
		var_86_0.displayDemo = true

		CloverElements.gametitlelabel:disable()
	end

	var_0_1.EnableAutoplayMark(true)

	if store.setting.burn_inPreviention and HOST_PLATFORM_IS_LINUX then
		mcp.set_dimming_timer(0)
	end
end

function var_0_1.GetStartState()
	local var_87_0 = CloverDemo
	local var_87_1 = ""

	local function var_87_2()
		if CLOVER_IS_DEBUG and debug_store.debugNoDisplayDemo then
			return false
		end

		return CloverDemo.displayDemo
	end

	return var_87_0.bootAborted and "cancel" or var_87_2() and "play_display" or var_0_1.GetStartState_OnlyDemo()
end

function var_0_1.GetStartState_OnlyDemo()
	local var_89_0 = CloverDemo
	local var_89_1 = ""

	if var_89_0.demo_phase == var_0_0.Phase.Auto then
		if var_89_0.bootDemo then
			if var_0_1.IsSwitchMyplaydemo() then
				var_89_1 = "switch_demo"
			else
				var_89_1 = "next_auto"
			end
		elseif store.autoplay.to_next_index then
			var_89_1 = "next_auto"
		else
			var_89_1 = "play_auto"
		end
	elseif var_89_0.demo_phase == var_0_0.Phase.Myplay then
		if var_89_0.bootDemo then
			if var_0_1.IsSwitchAutoplaydemo() then
				var_89_1 = "switch_demo"
			else
				var_89_1 = "next_myplay"
			end
		else
			var_89_1 = "play_myplay"
		end
	else
		assert(false)
	end

	print("next demo :" .. var_89_1)

	return var_89_1
end

function var_0_1.SetTitleSort()
	local var_90_0

	if CloverDemo.demo_phase == var_0_0.Phase.Auto then
		if store.autoplay then
			var_90_0 = store.autoplay.current_game_code
		end
	elseif CloverDemo.demo_phase == var_0_0.Phase.Myplay then
		var_90_0 = var_0_1.GetNextMyplayTitle()
	else
		assert(false)
	end

	CloverElements.gametitlelist:setSort(1, true, var_90_0)
	system.game_card:setCursor()
end

function var_0_1.EnableAutoplayMark(arg_91_0)
	if not CloverElements.autoplayMark then
		return
	end

	if arg_91_0 then
		CloverElements.autoplayMark:enable()
		var_0_1.UpdateAutoplayMark()
	else
		CloverElements.autoplayMark:disable()
	end
end

function var_0_1.UpdateAutoplayMark()
	local var_92_0 = CloverDemo

	CloverElements.autoplayMark.auto_demo_label:disable()
	CloverElements.autoplayMark.myplay_demo_label:disable()

	if var_92_0.demo_phase == var_0_0.Phase.Auto then
		CloverElements.autoplayMark.auto_demo_label:enable()
	elseif var_92_0.demo_phase == var_0_0.Phase.Myplay then
		CloverElements.autoplayMark.myplay_demo_label:enable()
	end
end

function var_0_1.ResetDisplayCount()
	store.autoplay.displayOption = store.setting.display
	store.autoplay.displayOptionCount = CloverConst.Demo.DISPLAY_OPTION_COUNT[store.setting.display]
end

function var_0_1.CheckCancelImput()
	if CloverPad1P.down:isAnything() or CloverPad2P.down:isAnything() then
		return true
	end

	local function var_94_0(arg_95_0)
		if GUI:isControlDown(GUI.gameDevice, arg_95_0, GUI.KEY_THRESHOLD) then
			return true
		end

		if GUI:isControlDown(GAME_DEVICE_2, arg_95_0, GUI.KEY_THRESHOLD) then
			return true
		end

		return false
	end

	if var_94_0(BUTTON_GUIDE) or var_94_0(AXIS_LEFT_TRIGGER) or var_94_0(AXIS_RIGHT_TRIGGER) then
		return true
	end

	if KEYBOARD_DEVICE and KEYBOARD_KEY_VOLUME_UP and GUI:isKeyPressed(KEYBOARD_DEVICE, KEYBOARD_KEY_VOLUME_UP, GUI.KEY_THRESHOLD) then
		return true
	end

	return false
end

function var_0_1.GameCodeToIndex(arg_96_0)
	return table_find_if(CloverElements.gametitlelist.elementArray, function(arg_97_0)
		return arg_97_0.gameinfo.game_code == arg_96_0
	end)
end

function var_0_1.GetGameTitleNum()
	return #CloverElements.gametitlelist.elementArray
end

function var_0_1.GetNextGameCode(arg_99_0)
	local var_99_0 = var_0_1.GameCodeToIndex(arg_99_0) + 1

	if var_99_0 > var_0_1.GetGameTitleNum() then
		var_99_0 = 1
	end

	return CloverElements.gametitlelist.elementArray[var_99_0].gameinfo.game_code
end

function var_0_1.GetNextMyplayTitle()
	local var_100_0 = CloverDemo
	local var_100_1 = store.autoplay.myplayIndex

	return MyplayData.GetTitle(var_100_1)
end

function var_0_1.GetNextMyplaySuspentionPointIndex()
	local var_101_0 = CloverDemo
	local var_101_1 = store.autoplay.myplayIndex

	return MyplayData.GetSuspentionIndex(var_101_1)
end

function var_0_1.GetNextMyplaySuspentionPointPosX()
	local var_102_0 = var_0_1.GetNextMyplaySuspentionPointIndex()
	local var_102_1, var_102_2 = CloverElements.resumemenu.cardlist.elementArray[var_102_0]:getWorldPosition()

	return var_102_1
end

function var_0_1.IncrementMyplayIndex()
	assert(not MyplayData.IsEmpty())

	local var_103_0 = CloverDemo
	local var_103_1 = store.autoplay.myplayIndex + 1

	if var_103_1 > MyplayData.GetNum() then
		var_103_1 = 1
	end

	store.autoplay.myplayIndex = var_103_1
end

function var_0_1.IsEdgeSelect()
	local var_104_0, var_104_1 = CloverElements.gametitlelist.current:getWorldPosition()
	local var_104_2 = CloverLayout.GetFirstSelectableTitleX()
	local var_104_3 = CloverLayout.GetLastSelectableTitleX()

	if var_104_0 < var_104_2 + 1 then
		return true
	end

	if var_104_0 > var_104_3 - 1 then
		return true
	end

	return false
end

function var_0_1.IsRightEdgeSelect()
	local var_105_0, var_105_1 = CloverElements.gametitlelist.current:getWorldPosition()

	if var_105_0 > CloverLayout.GetLastSelectableTitleX() - 1 then
		return true
	end

	return false
end

function var_0_1.GetSelectLine()
	local var_106_0, var_106_1 = CloverElements.gametitlelist.current:getWorldPosition()

	return CloverLayout.GetSelectableTitleIndex(var_106_0)
end

function var_0_1.IsSwitchMyplaydemo()
	if MyplayData.IsEmpty() then
		return false
	end

	if store.setting.myplayDemo == false then
		return false
	end

	if CLOVER_IS_DEBUG and debug_store.debugDemoLoopCheck then
		return true
	end

	local var_107_0 = false
	local var_107_1 = var_0_1.GameCodeToIndex(store.autoplay.current_game_code) + 1

	if var_107_1 > var_0_1.GetGameTitleNum() then
		var_107_1 = 1
	end

	if var_0_1.GameCodeToIndex(store.autoplay.start_game_code) == var_107_1 then
		return true
	end

	return false
end

function var_0_1.IsSwitchAutoplaydemo()
	if store.setting.autoplayDemo == false then
		return false
	end

	if CLOVER_IS_DEBUG and debug_store.debugDemoLoopCheck then
		return true
	end

	if store.autoplay.myplayIndex == 1 then
		return true
	end

	return false
end

function var_0_1.FinishAutodemo()
	store.autoplay.start_game_code = nil

	if store.autoplay.to_next_index then
		store.autoplay.to_next_index = false

		local var_109_0 = store.autoplay.current_game_code

		store.autoplay.current_game_code = var_0_1.GetNextGameCode(var_109_0)
	end
end
