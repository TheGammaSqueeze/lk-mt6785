import bpy
s=bpy.context.scene
s.render.resolution_x=640
s.render.resolution_y=360
s.render.resolution_percentage=100
try: s.eevee.taa_render_samples=16
except Exception as e: print("eevee samples set failed", e)
s.render.image_settings.file_format='PNG'
s.render.image_settings.color_mode='RGB'
s.render.filepath='/tmp/gba_frames/f_'
s.frame_start=1; s.frame_end=299; s.frame_step=2   # 150 frames = 30fps of the 5s @60fps
bpy.ops.render.render(animation=True)
