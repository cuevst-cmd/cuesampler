const fs = require('fs');
const path = require('path');
const sharp = require('/Users/jerryvolpe/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/sharp');
const out = __dirname;
const root = path.resolve(out, '../../..');
const samplerPath = path.join(root,'assets/web-hero/cue-main.jpg');
const rackPath = '/Volumes/PMCO/REPOS/CUERACK/outputs/cuerack-instagram-carousel-sharp/cuerack-instagram-03.png';
const samplerUri = 'data:image/jpeg;base64,'+fs.readFileSync(samplerPath).toString('base64');
const rackUri = 'data:image/png;base64,'+fs.readFileSync(rackPath).toString('base64');
// Crop through SVG viewports: original screenshot pixels are embedded unchanged.
function crop(uri, iw, ih, x,y,w,h,cx,cy,cw,ch) {
 return `<svg x="${x}" y="${y}" width="${w}" height="${h}" viewBox="${cx} ${cy} ${cw} ${ch}" preserveAspectRatio="xMidYMid meet" overflow="hidden"><image href="${uri}" x="0" y="0" width="${iw}" height="${ih}"/></svg>`;
}
function header(file) {
 const svg=fs.readFileSync(path.join(out,file+'-gumroad-revamp.svg'),'utf8');
 const marker='<path d="M88 350H936"';
 const start=svg.indexOf(marker);
 const end=svg.indexOf('/>',start)+2;
 return svg.slice(0,end);
}
(async()=>{
 const sm=await sharp(samplerPath).metadata();
 const rm=await sharp(rackPath).metadata();
 let sampler = header('cuesampler');
 // Enlarged central Chop Station section retains real slice colors and selection.
 sampler += `<rect x="80" y="410" width="864" height="542" rx="10" fill="#221F17"/>`;
 sampler += crop(samplerUri,sm.width,sm.height,88,418,848,308,230,115,760,276);
 // Actual zoom, scroll, tempo and pitch controls.
 sampler += crop(samplerUri,sm.width,sm.height,298,758,428,98,480,412,284,65);
 // The real keybed, centered on the played orange note.
 sampler += `<rect x="80" y="878" width="864" height="74" rx="5" fill="#1C211F"/>`;
 sampler += crop(samplerUri,sm.width,sm.height,88,886,848,58,235,567,804,55);
 sampler += '</svg>';
 let rack = header('cuerack');
 // Actual EQ and compressor panels from repository promotional screenshots.
 rack += crop(rackUri,rm.width,rm.height,88,441,450,450,73,277,502,502);
 rack += crop(rackUri,rm.width,rm.height,578,441,358,450,702,277,307,386);
 rack += '</svg>';
 for(const [name,svg] of [['cuesampler',sampler],['cuerack',rack]]) {
  const file=path.join(out,name+'-gumroad-real-ui');
  fs.writeFileSync(file+'.svg',svg);
  await sharp(Buffer.from(svg)).png().toFile(file+'.png');
  const m=await sharp(file+'.png').metadata();
  console.log(`${name}: ${m.width} x ${m.height}, ${m.format}`);
 }
 fs.writeFileSync(path.join(out,'real-ui-sources.txt'),`CueSampler: ${samplerPath}\nChop Station waveform, Zoom / Scroll / Tempo / Pitch controls, piano keyboard.\n\nCueRack: ${rackPath}\nEQ and Compressor panel crops.\n\nThe SVGs embed the original screenshots without repainting or regenerating their controls. The 1024 x 1024 PNGs are rendered from the SVG compositions. Earlier designs remain alongside these versions.\n`);
})();
