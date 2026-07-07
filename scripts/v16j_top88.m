% v16j_top88.m -- Phase 3.1.6 v16-j: the reference. Andreassen et al. (2011) "top88" 88-line SIMP topology
% optimization (ft=1 sensitivity filter), run on the same MBB beam as external/crd_v16j_topopt_bench.cpp. Prints the
% final compliance + wall time for parity + the speed crush. Run:  matlab -batch "run('scripts/v16j_top88.m')"
function v16j_top88()
    for scale = [1 2]
        run_top88(60*scale, 20*scale, 0.5, 3.0, 1.5, 1);
    end
end

function run_top88(nelx, nely, volfrac, penal, rmin, ft)
    t = tic;
    E0 = 1; Emin = 1e-9; nu = 0.3;
    A11 = [12  3 -6 -3;  3 12  3  0; -6  3 12 -3; -3  0 -3 12];
    A12 = [-6 -3  0  3; -3 -6 -3 -6;  0 -3 -6  3;  3 -6  3 -6];
    B11 = [-4  3 -2  9;  3 -4 -9  4; -2 -9 -4 -3;  9  4 -3 -4];
    B12 = [ 2 -3  4 -9; -3  2  9 -2;  4  9  2  3; -9 -2  3  2];
    KE = 1/(1-nu^2)/24*([A11 A12;A12' A11]+nu*[B11 B12;B12' B11]);
    nodenrs = reshape(1:(1+nelx)*(1+nely),1+nely,1+nelx);
    edofVec = reshape(2*nodenrs(1:end-1,1:end-1)+1,nelx*nely,1);
    edofMat = repmat(edofVec,1,8)+repmat([0 1 2*nely+[2 3 0 1] -2 -1],nelx*nely,1);
    iK = reshape(kron(edofMat,ones(8,1))',64*nelx*nely,1);
    jK = reshape(kron(edofMat,ones(1,8))',64*nelx*nely,1);
    F = sparse(2,1,-1,2*(nely+1)*(nelx+1),1);
    U = zeros(2*(nely+1)*(nelx+1),1);
    fixeddofs = union([1:2:2*(nely+1)],[2*(nelx+1)*(nely+1)]);
    alldofs = 1:2*(nely+1)*(nelx+1);
    freedofs = setdiff(alldofs,fixeddofs);
    % sensitivity/density filter (radius rmin)
    iH = ones(nelx*nely*(2*(ceil(rmin)-1)+1)^2,1); jH = ones(size(iH)); sH = zeros(size(iH)); k = 0;
    for i1 = 1:nelx
      for j1 = 1:nely
        e1 = (i1-1)*nely+j1;
        for i2 = max(i1-(ceil(rmin)-1),1):min(i1+(ceil(rmin)-1),nelx)
          for j2 = max(j1-(ceil(rmin)-1),1):min(j1+(ceil(rmin)-1),nely)
            e2 = (i2-1)*nely+j2; k = k+1; iH(k)=e1; jH(k)=e2;
            sH(k) = max(0,rmin-sqrt((i1-i2)^2+(j1-j2)^2));
          end
        end
      end
    end
    H = sparse(iH,jH,sH); Hs = sum(H,2);
    x = repmat(volfrac,nely,nelx); xPhys = x; loop = 0; change = 1;
    while change > 0.01 && loop < 200
      loop = loop+1;
      sK = reshape(KE(:)*(Emin+xPhys(:)'.^penal*(E0-Emin)),64*nelx*nely,1);
      K = sparse(iK,jK,sK); K = (K+K')/2;
      U(freedofs) = K(freedofs,freedofs)\F(freedofs);
      ce = reshape(sum((U(edofMat)*KE).*U(edofMat),2),nely,nelx);
      c = sum(sum((Emin+xPhys.^penal*(E0-Emin)).*ce));
      dc = -penal*(E0-Emin)*xPhys.^(penal-1).*ce;
      dv = ones(nely,nelx);
      if ft == 1
        dc(:) = H*(x(:).*dc(:))./Hs./max(1e-3,x(:));
      elseif ft == 2
        dc(:) = H*(dc(:)./Hs); dv(:) = H*(dv(:)./Hs);
      end
      l1 = 0; l2 = 1e9; move = 0.2;
      while (l2-l1)/(l1+l2) > 1e-3
        lmid = 0.5*(l2+l1);
        xnew = max(0,max(x-move,min(1,min(x+move,x.*sqrt(-dc./dv/lmid)))));
        if ft == 1, xPhys = xnew; elseif ft == 2, xPhys(:) = (H*xnew(:))./Hs; end
        if sum(xPhys(:)) > volfrac*nelx*nely, l1 = lmid; else, l2 = lmid; end
      end
      change = max(abs(xnew(:)-x(:))); x = xnew;
    end
    fprintf('nelx=%-3d nely=%-2d  final compliance = %.6f   vol=%.4f   >> top88 (MATLAB): %.0f ms <<\n', ...
            nelx, nely, c, mean(xPhys(:)), toc(t)*1000);
end
