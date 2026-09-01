require("/scripts/core/core.lua")

snd_component = class(SoundComponent)

function snd_component.play(arg_1_0)
	if arg_1_0.isBGM then
		if not debug_store.debugBGMOff then
			SoundComponent.play(arg_1_0)
		end
	elseif not debug_store.debugSEOff then
		SoundComponent.play(arg_1_0)
	end
end
