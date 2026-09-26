const fs = require('fs');
const path = require('path');
const sharp = require('/Users/jerryvolpe/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/sharp');
const out = __dirname;
const root = path.resolve(out, '../../..');
const samplerLogo = fs.readFileSync(path.join(root, 'assets/branding/cue_logo.svg'), 'utf8');
const rackLogo = fs.readFileSync('/Volumes/PMCO/REPOS/CUERACK/Source/Assets/cue_logo.svg', 'utf8');
const ink = '#1C211F', paper = '#F1EDE2', orange = '#FF6B24';
function logo(source) {
  const d = source.match(/<path[^>]*d="([^"]+)"/)[1];
  return `<svg x="88" y="81" width="516" height="96" viewBox="20 -700 4123 760"><path fill="${ink}" d="${d}"/></svg>`;
}
function shell(name, bg, content, source) {
 return `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">
 <rect width="1024" height="1024" fill="${bg}"/>
 ${logo(source)}
 <text x="88" y="300" fill="${ink}" font-family="Helvetica Neue, Helvetica, Arial, sans-serif" font-weight="800" font-size="108" letter-spacing="-4">${name}</text>
 <path d="M88 350H936" stroke="${ink}" stroke-width="2" opacity=".24"/>
 ${content}
 </svg>`;
}
const heights=[42,70,116,196,276,352,292,226,152,104,156,244,322,394,344,264,186,132,90,144,216,278,218,160,110,68,100,152,208,144,84,44];
let wave = '';
// A single highlighted slice is a sampling cue, drawn with a continuous waveform.
wave += `<rect x="304" y="425" width="206" height="476" rx="8" fill="${orange}"/>`;
for (let i=0; i<heights.length; i++) {
 const h=heights[i];
 wave += `<rect x="${92+i*26.5}" y="${663-h/2}" width="17" height="${h}" rx="8.5" fill="${ink}"/>`;
}
wave += `<path d="M304 407V393H326 M488 393H510V407 M304 919V933H326 M488 933H510V919" fill="none" stroke="${ink}" stroke-width="4"/>`;
// Fine cut lines preserve the sliced-sample metaphor without UI text.
wave += `<path d="M564 440V886 M775 440V886" stroke="${ink}" stroke-width="2" stroke-dasharray="4 10" opacity=".23"/>`;
let rack = '';
for(let row=0;row<3;row++) {
 const y=408+row*173;
 rack += `<rect x="88" y="${y}" width="848" height="149" rx="15" fill="${ink}"/>`;
 // Rack screws.
 for(const x of [109,915]) for(const dy of [22,127]) rack += `<circle cx="${x}" cy="${y+dy}" r="4" fill="${paper}" opacity=".42"/>`;
 // Signal bars.
 for(let j=0;j<8;j++) rack += `<rect x="${143+j*31}" y="${y+55}" width="18" height="40" rx="3" fill="${j<6-row ? paper:'#414540'}"/>`;
 // Small status accent.
 rack += `<rect x="437" y="${y+61}" width="71" height="27" rx="13.5" fill="${orange}"/>`;
 // Two flat controls, with clear pointer positions.
 for(let k=0;k<2;k++) {
 const cx=629+k*164, cy=y+75, a=(-50+row*47+k*56)*Math.PI/180;
 rack+=`<circle cx="${cx}" cy="${cy}" r="47" fill="${k===0?paper:orange}"/><path d="M${cx+Math.sin(a)*22} ${cy-Math.cos(a)*22}L${cx+Math.sin(a)*36} ${cy-Math.cos(a)*36}" stroke="${ink}" stroke-width="7" stroke-linecap="round"/>`;
 }
}
(async()=>{
 for(const [file,name,bg,content,source] of [
  ['cuesampler-gumroad-revamp','SAMPLER',paper,wave,samplerLogo],
  ['cuerack-gumroad-revamp','RACK',orange,rack,rackLogo]
 ]) {
  const svg=shell(name,bg,content,source);
  fs.writeFileSync(path.join(out,file+'.svg'),svg);
  await sharp(Buffer.from(svg)).png().toFile(path.join(out,file+'.png'));
  const m=await sharp(path.join(out,file+'.png')).metadata();
  console.log(file+': '+m.width+' × '+m.height+', '+m.format);
 }
})();
