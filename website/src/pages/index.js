import React, {useState} from 'react';
import Layout from '@theme/Layout';
import Link from '@docusaurus/Link';
import useBaseUrl from '@docusaurus/useBaseUrl';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import {content} from '../home-content';
import styles from './index.module.css';

const releases = 'https://github.com/atomsprout/nestvane/releases';
const repository = 'https://github.com/atomsprout/nestvane';
const video = 'wDVr3-UY1Ec';

function Shot({file, width, height, alt, caption}) {
  return (
    <figure className={styles.shot}>
      <img src={useBaseUrl(`img/${file}`)} width={width} height={height} alt={alt} loading="lazy" decoding="async" />
      <figcaption>{caption}</figcaption>
    </figure>
  );
}

// The picture is the site's own file. YouTube is contacted only after the visitor presses play.
function Video({label, alt, watch}) {
  const [playing, setPlaying] = useState(false);
  const poster = useBaseUrl('img/nestvane-video.jpg');
  return (
    <div className={styles.videoWrap}>
      <div className={styles.video}>
        {playing ? (
          <iframe
            src={`https://www.youtube-nocookie.com/embed/${video}?autoplay=1&playsinline=1`}
            title={label}
            allow="autoplay; encrypted-media; picture-in-picture; fullscreen"
            allowFullScreen
          />
        ) : (
          <button type="button" className={styles.videoButton} onClick={() => setPlaying(true)} aria-label={label}>
            <img src={poster} width="540" height="960" alt={alt} />
            <span className={styles.play} aria-hidden="true" />
          </button>
        )}
      </div>
      <p className={styles.fine}>
        <a href={`https://www.youtube.com/shorts/${video}`}>{watch}</a>
      </p>
    </div>
  );
}

export default function Home() {
  const {i18n} = useDocusaurusContext();
  const text = content[i18n.currentLocale] ?? content.en;
  const icon = useBaseUrl('img/nestvane-icon.png');
  const diagram = useBaseUrl('img/architecture.svg');
  return (
    <Layout title={text.title} description={text.description}>
      <header className={styles.hero}>
        <div className="container">
          <img className={styles.icon} src={icon} width="128" height="128" alt={text.iconAlt} />
          <p className={styles.status}>{text.status}</p>
          <h1 className={styles.name}>Nestvane</h1>
          <p className={styles.tagline}>{text.tagline}</p>
          <p className={styles.lede}>{text.lede}</p>
          <div className={styles.actions}>
            <Link className="button button--primary button--lg" href={releases}>{text.download}</Link>
            <Link className="button button--secondary button--lg" to="/docs/">{text.readDocs}</Link>
            <Link className="button button--outline button--secondary button--lg" href={repository}>{text.source}</Link>
          </div>
          <p className={styles.note}>
            {text.downloadNote} <Link to="/docs/installing/">{text.howToInstall}</Link>
          </p>
        </div>
      </header>

      <main>
        <section className={styles.section} id="video">
          <div className="container">
            <h2>{text.videoTitle}</h2>
            <p className={styles.intro}>{text.videoIntro}</p>
            <Video label={text.videoPlay} alt={text.videoAlt} watch={text.videoWatch} />
          </div>
        </section>

        <section className={`${styles.section} ${styles.alt}`} id="screenshots">
          <div className="container">
            <h2>{text.shotsTitle}</h2>
            <p className={styles.intro}>{text.shotsIntro}</p>
            <div className={styles.shots} role="group" aria-label={text.shotsTitle} tabIndex={0}>
              {text.shots.map(shot => <Shot key={shot.file} {...shot} />)}
            </div>
          </div>
        </section>

        <section className={styles.section} id="works">
          <div className="container">
            <h2>{text.worksTitle}</h2>
            <p className={styles.intro}>{text.worksIntro}</p>
            <ul className={styles.cards}>
              {text.works.map(([heading, body]) => (
                <li key={heading} className={styles.card}><h3>{heading}</h3><p>{body}</p></li>
              ))}
            </ul>
            <dl className={styles.figures}>
              {text.figures.map(([label, value]) => (
                <div key={label}><dt>{label}</dt><dd>{value}</dd></div>
              ))}
            </dl>
            <p className={styles.fine}>
              {text.figuresNote} <Link to="/docs/measurements/">{text.figuresMore}</Link>
            </p>
          </div>
        </section>

        <section className={`${styles.section} ${styles.alt}`} id="how">
          <div className="container">
            <h2>{text.howTitle}</h2>
            <p className={styles.intro}>{text.howIntro}</p>
            <ol className={styles.cards}>
              {text.pieces.map(([heading, body]) => (
                <li key={heading} className={styles.card}><h3>{heading}</h3><p>{body}</p></li>
              ))}
            </ol>
            <img className={styles.diagram} src={diagram} alt={text.diagramAlt} loading="lazy" />
            <p className={styles.fine}>
              {text.howMore} <Link to="/docs/architecture/">{text.howMoreLink}</Link>
            </p>
          </div>
        </section>

        <section className={styles.section} id="limits">
          <div className="container">
            <h2>{text.limitsTitle}</h2>
            <ul className={styles.plain}>
              {text.limits.map(item => <li key={item}>{item}</li>)}
            </ul>
            <div className={styles.card}>
              <h3>{text.requirementsTitle}</h3>
              {text.requirements.map(item => <p key={item}>{item}</p>)}
            </div>
          </div>
        </section>

        <section className={`${styles.section} ${styles.alt}`} id="thanks">
          <div className="container">
            <h2>{text.thanksTitle}</h2>
            <p className={styles.intro}>
              {text.thanks} <Link to="/docs/comparison/">{text.thanksLink}</Link>
            </p>
          </div>
        </section>

        <section className={styles.section} id="about">
          <div className={`container ${styles.prose}`}>
            <h2>{text.aboutTitle}</h2>
            {text.about.map(paragraph => <p key={paragraph}>{paragraph}</p>)}
          </div>
        </section>

        <section className={`${styles.section} ${styles.alt}`} id="community">
          <div className="container">
            <h2>{text.communityTitle}</h2>
            <p className={styles.intro}>{text.communityIntro}</p>
            <ul className={styles.cards}>
              {text.channels.map(([name, address, href, body]) => (
                <li key={name} className={styles.card}>
                  <h3><a href={href}>{name}</a></h3>
                  <p className={styles.address}>{address}</p>
                  <p>{body}</p>
                </li>
              ))}
            </ul>
          </div>
        </section>

        {/* The iOS app links to #privacy. Keep this id, and keep the English policy text word for word. */}
        <section className={styles.section} id="privacy">
          <div className={`container ${styles.prose}`}>
            <h2>{text.privacyTitle}</h2>
            <p className={styles.intro}>{text.privacyEffective}</p>
            {text.privacy.map(([heading, body]) => (
              <React.Fragment key={heading}><h3>{heading}</h3><p>{body}</p></React.Fragment>
            ))}
            {text.privacyNote && <p className={styles.fine}>{text.privacyNote}</p>}
          </div>
        </section>
      </main>
    </Layout>
  );
}
