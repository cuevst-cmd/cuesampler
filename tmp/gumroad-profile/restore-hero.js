const fs = require('fs');

const profilePath = '/Users/jerryvolpe/Documents/SAMPLERv3/tmp/gumroad-profile/profile.html';
const backdropPath = '/Users/jerryvolpe/Documents/SAMPLERv3/assets/web-hero/cue-original-composition.jpg';
const mainPath = '/Users/jerryvolpe/Documents/SAMPLERv3/assets/web-hero/cue-main.jpg';

const backdrop = fs.readFileSync(backdropPath).toString('base64');
const main = fs.readFileSync(mainPath).toString('base64');

let html = fs.readFileSync(profilePath, 'utf8');
const heroPattern = /  <section class="store-hero"[\s\S]*?<\/section>\n  <section id="products"/;
const hero = `  <section class="store-hero" aria-label="CUE audio software">
    <div class="wrap">
      <div class="hero-frame">
        <div class="hero-stage">
          <div class="hero-artboard">
            <img class="hero-backdrop" src="data:image/jpeg;base64,${backdrop}" alt="" width="1350" height="638">
            <div class="hero-panel-main">
              <img src="data:image/jpeg;base64,${main}" alt="CUE Sampler interface" width="1240" height="679">
            </div>
          </div>
        </div>
        <div class="hero-copy">
          <p class="hero-title">Sample Smarter</p>
          <p class="hero-subtitle">New Beta Coming Soon</p>
        </div>
      </div>
    </div>
  </section>
  <section id="products"`;

if (!heroPattern.test(html)) {
  throw new Error('Could not locate the current hero section.');
}

html = html.replace(heroPattern, hero);
fs.writeFileSync(profilePath, html);
