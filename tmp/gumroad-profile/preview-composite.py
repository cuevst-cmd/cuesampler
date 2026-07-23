from PIL import Image

backdrop = Image.open('/Users/jerryvolpe/Documents/SAMPLERv3/assets/web-hero/cue-original-composition.jpg').convert('RGB')
main = Image.open('/Users/jerryvolpe/Documents/SAMPLERv3/assets/web-hero/cue-main.jpg').convert('RGB')

target_width = round(backdrop.width * 0.588)
target_height = round(target_width / (main.width / main.height))
main = main.resize((target_width, target_height), Image.Resampling.LANCZOS)
x = round((backdrop.width - target_width) / 2)
y = round(backdrop.height * 0.256)
backdrop.paste(main, (x, y))
backdrop.save('/Users/jerryvolpe/Documents/SAMPLERv3/tmp/gumroad-profile/restored-hero-preview.jpg', quality=90)
