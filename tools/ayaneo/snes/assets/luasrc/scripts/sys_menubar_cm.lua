require("/scripts/core/core.lua")
require("/scripts/app/clover_const.lua")

sys_menubar_cm = class(GUIButton)

function sys_menubar_cm.start(arg_1_0)
	GUIButton.start(arg_1_0)
	arg_1_0.caption:disable()
	arg_1_0.caption:setLocalScale(0, 0)
	arg_1_0.active_component:setAlpha(0)

	arg_1_0.caption_b_component = arg_1_0.caption_body:getComponent(VisualComponent)
	arg_1_0.caption_l_component = arg_1_0.caption_l:getComponent(VisualComponent)
	arg_1_0.caption_r_component = arg_1_0.caption_r:getComponent(VisualComponent)

	-- the authored caption label colour is a mid grey (0.298) which is nearly invisible on
	-- the caption's dark bubble; the reference draws the caption text white.
	if arg_1_0.caption_label then
		local var_1_0 = arg_1_0.caption_label:getComponent(LabelComponent) or arg_1_0.caption_label

		var_1_0:setColor(1, 1, 1, 1)
	end
end

function sys_menubar_cm.activate(arg_2_0)
	if arg_2_0.tween and TweenManager:isTweenActive(arg_2_0.tween) then
		TweenManager:stopTween(arg_2_0.tween)
	end

	arg_2_0:calcLength()

	local var_2_0 = store.autoplay and store.autoplay.running

	arg_2_0.caption:setZIndex(3)

	arg_2_0.tween = Tween:parallel(Tween:scaleTo(arg_2_0.button, 0.2, 1.2, 1.2, Ease.outExpo), Tween:sequence(Tween:wait(CloverConst.Cursor.MENUBAR_CAPTION_DELAY), Tween:scaleTo(arg_2_0.caption, 0.2, 1, 1, Ease.outExpo)), Tween:worldNodeEnabledTo(arg_2_0.caption, 0.05, not var_2_0), Tween:alphaTo(arg_2_0.active_component, 0, 1)):start()

	if system.cursor then
		system.cursor:setNextStartZ(CloverConst.Cursor.MENUBAR_PRIORITY_Z)
		system.cursor:setSquare(arg_2_0.active_component)
		system.cursor:setNextStartZ(0)
	end
end

function sys_menubar_cm.deactivate(arg_3_0)
	if arg_3_0.tween and TweenManager:isTweenActive(arg_3_0.tween) then
		TweenManager:stopTween(arg_3_0.tween)
	end

	arg_3_0.caption:setZIndex(1)

	arg_3_0.tween = Tween:parallel(Tween:scaleTo(arg_3_0.button, 0.2, 1, 1, Ease.inExpo), Tween:scaleTo(arg_3_0.caption, 0.2, 0, 0, Ease.inExpo), Tween:worldNodeEnabledTo(arg_3_0.caption, 0.15, false), Tween:alphaTo(arg_3_0.active_component, 0, 0)):start()
end

function sys_menubar_cm.calcLength(arg_4_0)
	if arg_4_0.caption_label then
		local var_4_0, var_4_1 = arg_4_0.caption_label:getSize()
		local var_4_2, var_4_3 = arg_4_0.caption_b_component:getSize()

		if var_4_0 < var_4_2 then
			var_4_0 = var_4_2
		end

		arg_4_0.caption_b_component:setSize(var_4_0, var_4_3)

		local var_4_4, var_4_5 = arg_4_0.caption_body:getLocalPosition()
		local var_4_6, var_4_7 = arg_4_0.caption_l_component:getSize()
		local var_4_8, var_4_9 = arg_4_0.caption_r_component:getSize()

		arg_4_0.caption_l:setLocalPosition(-(var_4_0 / 2 + var_4_6 / 2), var_4_5)
		arg_4_0.caption_r:setLocalPosition(var_4_0 / 2 + var_4_8 / 2, var_4_5)
	end
end
