import { lazy, StrictMode, Suspense } from 'react';
import { createRoot } from 'react-dom/client';
import { AppShell } from './components/AppShell';
import './styles/tokens.css';
import './styles/global.css';

const ComponentCatalog = lazy(() => import('./catalog/ComponentCatalog'));
const ScreenReview = lazy(() => import('./screens/ScreenReview'));
const view = new URLSearchParams(window.location.search).get('view');

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    {view === 'components' ? <Suspense fallback={<p role="status">Loading local component examples…</p>}><ComponentCatalog /></Suspense>
      : view === 'screens' ? <Suspense fallback={<p role="status">Loading local screen examples…</p>}><ScreenReview /></Suspense> : <AppShell />}
  </StrictMode>,
);
