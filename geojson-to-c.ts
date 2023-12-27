const fs = require('fs');

const geo = JSON.parse(fs.readFileSync(process.argv[2], 'utf-8'));

console.log(`MultiPolygon mp;`);
for (const poly of geo.features[0].geometry.coordinates) {
	console.log(`{`);
	console.log(`Polygon p;`);
	for (const [lon, lat] of poly[0]) {
		console.log(`a(p, ${lon}, ${lat});`);
	}
	console.log(`mp.push_back(p);`);
	console.log(`}`);
};
