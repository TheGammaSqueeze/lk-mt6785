import bpy
s = bpy.context.scene
r = s.render
r.resolution_x = 640
r.resolution_y = 480          # 4:3, half of the 1280x960 panel
r.resolution_percentage = 100
r.film_transparent = True     # keep alpha so the encoder can composite white or black
r.image_settings.file_format = 'PNG'
r.image_settings.color_mode = 'RGBA'
try:
    s.eevee.taa_render_samples = 16
except Exception as e:
    print("eevee samples set failed", e)
# keep the horizontal field of view fixed so 4:3 shows more top/bottom
# (un-crop) instead of cropping the 16:9 framing.
if s.camera:
    s.camera.data.sensor_fit = 'HORIZONTAL'
r.filepath = '/tmp/gba43/f_'
s.frame_start = 1
s.frame_end = 300
s.frame_step = 1              # 60fps
bpy.ops.render.render(animation=True)
