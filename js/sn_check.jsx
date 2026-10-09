import React, { useState, useEffect } from 'react';
import { Scanner  } from '@yudiel/react-qr-scanner';

export default function SnCheck() {
  const [input, setInput] = useState('');
  const [result, setResult] = useState(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(null);
  const [showScanner, setShowScanner] = useState(false);


  // URL-Parameter auslesen beim Laden der Komponente
  useEffect(() => {
    const params = new URLSearchParams(window.location.search);
    const urlInput = params.get('sn');
    if (urlInput) {
      setInput(urlInput);
    }
  }, []);


  const handleSubmit = async (e) => {
    e.preventDefault();
    setLoading(true);
    setError(null);
    setResult(null);

    try {
      const response = await fetch(`https://cdn.code7.io/pcb/pg.php?q=${encodeURIComponent(input)}`);
      if (!response.ok) throw new Error('API request failed');

      const data = await response.json();
      setResult(data);
    } catch (err) {
      setError(err.message || 'Something went wrong');
    } finally {
      setLoading(false);
    }
  };

  const handleScanResult = (value) => {
    if (value[0].format === 'qr_code') {
      const qr = value[0].rawValue;
      setInput(qr);
      setShowScanner(false); // Close scanner after successful scan
    }
  };

  return (
    <div style={{ margin: '2em 0' }}>
      <form onSubmit={handleSubmit}>
        <input type="text" value={input} onChange={(e) => setInput(e.target.value)} placeholder="" style={{ padding: '0.9em', marginRight: '1em', borderRadius: 'var(--ifm-button-border-radius)' }} />
        <button type="submit" disabled={loading} class="button button--lg button--primary">
          {loading ? 'Loading...' : 'Check!'}
        </button>
        <button type="button" onClick={() => setShowScanner(!showScanner)} className="button button--lg button--secondary" style={{ marginLeft: '1em' }} >
          {showScanner ? 'Close Scanner' : 'Scan QR'}
        </button>
      </form>

      {showScanner && (
        <div style={{ marginTop: '1em', maxWidth: '400px' }}>
          <Scanner 
            onScan={handleScanResult}
            onError={(err) => setError('Camera error or permission denied')}
            constraints={{ facingMode: 'environment' }}
          />
        </div>
      )}

      {result && (
        <div style={{ marginTop: '1em' }}>
          <strong>Result:</strong>
          <pre>
            Status: { result.status == 'valid' ? '✅ Valid' : '❌ Invalid' }<br />
            Production Date: {result.pd}<br />
            Hardware Revision: {result.hw}<br />
            Batch: {result.batch}<br />
          </pre>
        </div>
      )}

      {error && (
        <div style={{ marginTop: '1em', color: 'red' }}>
          <strong>Error:</strong> {error}
        </div>
      )}
    </div>
  );
}
